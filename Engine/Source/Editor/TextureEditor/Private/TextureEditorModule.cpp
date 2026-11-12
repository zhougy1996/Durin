#include "TextureEditorModule.h"
#include "TextureBuildSettingsDetails.h"

#include "ContentBrowser/ContentBrowserContracts.h"
#include "Icons/FontAwesomeIcons.h"
#include "ContentBrowser/TextureCubeDetails.h"
#include "Texture2DPropertyEditing.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"
#include "Texture/VolumeTexture.h"
#include "Thumbnail/ThumbnailManager.h"
#include "Thumbnail/TextureThumbnailRenderer.h"
#include "Thumbnail/TextureCubeThumbnailRenderer.h"
#include "Import/TextureFileImport.h"
#include "Dialogs/FileDialog.h"
#include "Project/Project.h"
#include "MonaImGui.h"
#include "Asset/Load.h"
#include "Editor/EditorEngine.h"
#include "Notifications/Notification.h"
#include "DObject/WeakObjectPtr.h"

namespace Durin
{
	using namespace TextureEditor;
	IMPLEMENT_MODULE(FTextureEditorModule, TextureEditor)

	struct FTextureEditorModule::FIntegrationState
	{
		std::vector<ContentBrowser::FScopedExtensionRegistration> TypeExtras;
		ContentBrowser::FScopedExtensionRegistration ImportExtension;
		ContentBrowser::FScopedExtensionRegistration RetrySaveExtension;
		std::shared_ptr<TextureEditor::FTextureFileImport> FileImport;
		std::string LastSourceDirectory;
		FNotificationId ImportProgress = 0;
		TWeakObjectPtr<DEditorEngine> ImportEditor;
		std::optional<std::weak_ptr<void>> OwnerLifetime;
	};

	FTextureEditorModule::FTextureEditorModule()
		: Integration(std::make_unique<FIntegrationState>())
	{
	}
	FTextureEditorModule::~FTextureEditorModule() = default;

	auto FTextureEditorModule::StartupModule() -> void
	{
		require(TextureEditor::RegisterTexture2DPropertyEditing());
		DetailCustomizationRegistration = FDetailCustomizationRegistry::Get().RegisterDetailCustomization(
			DTexture2D::StaticClass(), TextureEditor::CreateTextureBuildSettingsDetails());
		require(static_cast<bool>(DetailCustomizationRegistration));

		require(RegisterContributions(GetDefaultThumbnailManager()));
	}

	auto FTextureEditorModule::ShutdownModule() -> void
	{
		UnregisterContributions();
		FDetailCustomizationRegistry::Get().Unregister(DetailCustomizationRegistration);
		DetailCustomizationRegistration = {};
		TextureEditor::UnregisterTexture2DPropertyEditing();
	}

	auto FTextureEditorModule::RegisterContributions(DThumbnailManager& ThumbnailManager) -> bool
	{
		check(!Texture2DThumbnailRegistration && !TextureCubeThumbnailRegistration);
		Integration->FileImport = std::make_shared<TextureEditor::FTextureFileImport>();

		auto Texture2DHandle = ThumbnailManager.RegisterScoped(
			std::make_unique<DTextureThumbnailRenderer>());
		if (!Texture2DHandle)
		{
			UnregisterContributions();
			return false;
		}
		Texture2DThumbnailRegistration =
			std::make_unique<FThumbnailRendererRegistrationHandle>(
				std::move(*Texture2DHandle));
		auto TextureCubeHandle = ThumbnailManager.RegisterScoped(
			std::make_unique<DTextureCubeThumbnailRenderer>());
		if (!TextureCubeHandle)
		{
			UnregisterContributions();
			return false;
		}
		TextureCubeThumbnailRegistration =
			std::make_unique<FThumbnailRendererRegistrationHandle>(
				std::move(*TextureCubeHandle));
		std::string Error;
		auto ImportExtension = ContentBrowser::RegisterExtension({
			.Id = "texture.import-texture",
			.Label = "From File...",
			.Category = ContentBrowser::EExtensionCategory::Import,
			.Order = 100,
			.Mutation = ContentBrowser::EContentMutation::MutatesContent,
			.IsApplicable = [this](const auto& Context) {
				return !Context.VirtualDirectory.empty() && !Integration->FileImport->IsRunning();
			},
			.Invoke = [this](const auto& Invocation) {
				const auto ImportCallbacks = ContentBrowser::MakeImportDialogCallbacks(Invocation, {});
				if (!Integration->FileImport->SetCallbacks(ImportCallbacks)) return;
				Integration->OwnerLifetime = Invocation.bHasHostLifetime ? std::optional(Invocation.HostLifetime) : std::nullopt;
				Integration->ImportEditor = GetTransactionEditor();
				FFileDialogRequest Request;
				Request.ParentWindowHandle = ImGui::GetMainViewport()->PlatformHandleRaw;
				Request.Title = "Import Textures From Files";
				Request.bAllowMultiple = true;
				Request.Filters = {{"Texture Images", "*.png;*.jpg;*.jpeg;*.bmp;*.tga"}};
				if (const FProjectInfo* Project = GetCurrentProject())
					Request.InitialDirectory = Project->ProjectDir;
				if (!Integration->LastSourceDirectory.empty())
					Request.InitialDirectory = Integration->LastSourceDirectory;
				const auto Selected = OpenFileDialog(Request);
				if (Selected.Status == EFileDialogStatus::Error)
					ImportCallbacks.Report(Selected.ErrorMessage);
				else if (Selected.Status == EFileDialogStatus::Selected)
				{
					Integration->LastSourceDirectory =
						std::filesystem::path(Selected.FilePath).parent_path().generic_string();
					if (!Integration->FileImport->Begin(Selected.FilePaths, Invocation.Context.VirtualDirectory)) return;
					if (auto* Owner = Integration->ImportEditor.Get())
						Integration->ImportProgress = Owner->GetNotificationManager().BeginProgress({
							.Message = "Importing textures...", .Progress = 0.0f,
							.Cancel = [Weak = std::weak_ptr(Integration->FileImport)] {
								if (auto Importer = Weak.lock()) Importer->Cancel();
							},
							.Presentation = ENotificationPresentation::StatusBar, .bRecordInHistory = false});
				}
			},
			.DrawHostPresentation = [this](const ContentBrowser::FExtensionHost& Host, bool bAllowMutation) {
				if (Integration->OwnerLifetime && (Integration->OwnerLifetime->owner_before(Host.Lifetime) || Host.Lifetime.owner_before(*Integration->OwnerLifetime))) return;
				auto& Importer = *Integration->FileImport;
				if (Integration->OwnerLifetime && Integration->OwnerLifetime->expired())
				{
					Importer.Cancel(); Importer.Tick(false);
					if (auto* Owner = Integration->ImportEditor.Get(); Owner && Integration->ImportProgress)
						Owner->GetNotificationManager().CancelProgress(Integration->ImportProgress, "Texture import host stopped.");
					Integration->ImportProgress = 0;
					if (!Importer.IsRunning())
					{ Integration->FileImport = std::make_shared<FTextureFileImport>(); Integration->OwnerLifetime.reset(); }
					return;
				}
				const bool bWasRunning = Importer.IsRunning();
				Importer.Tick(bAllowMutation);
				if (!bWasRunning) return;
				auto* Owner = Integration->ImportEditor.Get();
				if (!Owner) return;
				auto& Notifications = Owner->GetNotificationManager();
				if (Importer.IsRunning())
				{
					Notifications.UpdateProgress(Integration->ImportProgress,
						static_cast<float>(Importer.GetCompletedCount()) / Importer.GetTotalCount(),
						bAllowMutation ? Importer.GetActivity() : "Texture import paused");
					return;
				}
				const auto Summary = std::format("Textures: {} imported, {} failed, {} canceled.",
					Importer.GetSavedCount(), Importer.GetFailedCount(), Importer.GetCanceledCount());
				if (Importer.GetFailedCount()) Notifications.FailProgress(Integration->ImportProgress, Summary);
				else if (Importer.GetCanceledCount()) Notifications.CancelProgress(Integration->ImportProgress, Summary);
				else Notifications.CompleteProgress(Integration->ImportProgress, Summary);
				Integration->ImportProgress = 0;
				std::string Details;
				for (const auto& Error : Importer.GetErrors()) Details += Error + "\n";
				Details += Importer.GetTimingDetails();
				FNotificationDesc Result{
					.Type = Importer.GetFailedCount() ? ENotificationType::Warning
						: Importer.GetCanceledCount() ? ENotificationType::Info : ENotificationType::Success,
					.Message = Summary, .Details = std::move(Details)};
				if (Importer.HasPendingSaves())
					Result.Action = FNotificationAction{
						.Label = "Retry saves",
						.IsEnabled = [Weak = std::weak_ptr(Integration->FileImport)] {
							auto Importer = Weak.lock();
							return Importer && Importer->CanRetrySaves();
						},
						.Invoke = [Weak = std::weak_ptr(Integration->FileImport)] {
							if (auto Importer = Weak.lock()) Importer->RetryPendingSaves();
						}};
				Notifications.Post(std::move(Result));
			},
			.StopHostPresentation = [this](const std::weak_ptr<void>& Lifetime) {
				if (!Integration->OwnerLifetime || Integration->OwnerLifetime->owner_before(Lifetime) || Lifetime.owner_before(*Integration->OwnerLifetime)) return;
				if (auto* Owner = Integration->ImportEditor.Get(); Owner && Integration->ImportProgress)
					Owner->GetNotificationManager().CancelProgress(Integration->ImportProgress, "Texture import host stopped.");
				Integration->ImportProgress = 0;
				Integration->FileImport->Cancel();
				Integration->FileImport = std::make_shared<FTextureFileImport>();
				Integration->OwnerLifetime.reset();
			},
			}, Error);
		if (!ImportExtension.IsValid())
		{
			DURIN_ERROR("Could not register Content Browser Texture import: {}", Error);
			UnregisterContributions();
			return false;
		}
		Integration->ImportExtension = std::move(ImportExtension);
		Integration->RetrySaveExtension = ContentBrowser::RegisterExtension({
			.Id = "texture.retry-import-save",
			.Label = "Retry Texture Saves",
			.Category = ContentBrowser::EExtensionCategory::Import,
			.Order = 110,
			.Mutation = ContentBrowser::EContentMutation::MutatesContent,
			.IsApplicable = [this](const auto&) { return !Integration->FileImport->IsRunning() && Integration->FileImport->HasPendingSaves(); },
			.Invoke = [this](const auto& Invocation) {
				if (!Integration->FileImport->SetCallbacks(ContentBrowser::MakeImportDialogCallbacks(Invocation, {}))) return;
				Integration->OwnerLifetime = Invocation.bHasHostLifetime ? std::optional(Invocation.HostLifetime) : std::nullopt;
				Integration->ImportEditor = GetTransactionEditor();
				Integration->FileImport->RetryPendingSaves();
			},
		}, Error);
		if (!Integration->RetrySaveExtension.IsValid())
		{
			DURIN_ERROR("Could not register texture import save retry: {}", Error);
			UnregisterContributions();
			return false;
		}
		std::string PresentationError;

		{
			auto Handle = ContentBrowser::RegisterAssetTypeExtras({
				.AssetClassName = DTextureCube::StaticClass()->GetQualifiedName().ToString(),
				.ThumbnailBadgeIcon = Icons::Cube,
				.Details = TextureEditor::MakeTextureCubeDetailsProvider(),
			}, PresentationError);
			if (!Handle.IsValid())
			{
				DURIN_ERROR("Could not register browser type presentation: {}", PresentationError);
				UnregisterContributions();
				return false;
			}
			Integration->TypeExtras.push_back(std::move(Handle));
		}

		return true;
	}

	auto FTextureEditorModule::UnregisterContributions() -> void
	{
		Integration->TypeExtras.clear();
		Integration->ImportExtension.Reset();
		Integration->RetrySaveExtension.Reset();
		if (auto* Owner = Integration->ImportEditor.Get(); Owner && Integration->ImportProgress)
			Owner->GetNotificationManager().CancelProgress(Integration->ImportProgress, "Texture import stopped.");
		Integration->ImportProgress = 0;
		Integration->FileImport.reset();
		TextureCubeThumbnailRegistration.reset();
		Texture2DThumbnailRegistration.reset();
	}

}
