#include "Panels/SceneViewportPanel.h"

#include "Asset/Asset.h"
#include "Components/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor/AssetDragDrop.h"
#include "Editor/EditorEngine.h"
#include "Editor/Transaction.h"
#include "Editor/WorkspaceUI.h"
#include "Engine/Engine.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Actors/StaticMeshActor.h"
#include "Actors/SkeletalMeshActor.h"
#include "Actors/SkyBoxActor.h"
#include "Actors/TerrainActor.h"
#include "Components/TerrainComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Workspace/LevelEditorContext.h"
#include "Workspace/LevelEditorWorkspace.h"
#include "Math/Vector.h"
#include "Client/SceneViewport.h"
#include "Application/MonaApplication.h"
#include "MonaImGui.h"
#include "SceneViewProjection.h"
#include "Viewport/LevelEditorViewportClient.h"
#include "Viewport/CameraPreviewViewportClient.h"
#include "Viewport/ViewportPresentation.h"
#include "Widgets/MViewport.h"
#include "Widgets/MWindow.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"
#include "StaticMeshLevelMutations.h"
#include "TerrainPlacement.h"
#include "SkyBoxPlacement.h"
#include "Texture/TextureCube.h"
#include "Terrain/TerrainHeightmap.h"

namespace Durin::Editor::Level
{
	namespace
	{
		auto Add(const ImVec2& A, const ImVec2& B) -> ImVec2 { return ImVec2(A.x + B.x, A.y + B.y); }

		auto TryParseFloat(std::string_view Text, float& OutValue) -> bool
		{
			const auto [End, Error] = std::from_chars(Text.data(), Text.data() + Text.size(), OutValue);
			return Error == std::errc{} && End == Text.data() + Text.size() && std::isfinite(OutValue);
		}

		auto MakeUniqueActorName(DLevel& Level, FName Requested) -> FName
		{
			if (!Level.FindActorByName(Requested)) return Requested;
			const std::string Base = Requested.ToString();
			for (uint32 Suffix = 2;; ++Suffix)
			{
				FName Candidate(std::format("{}_{}", Base, Suffix));
				if (!Level.FindActorByName(Candidate)) return Candidate;
			}
		}
	} // namespace


	FSceneViewportPanel::FSceneViewportPanel(FModuleOwnedCallbackGate InOwnerGate)
		: OwnerGate(std::move(InOwnerGate))
		, PreferredPlayStartLocation(::Durin::Editor::EPlayStartLocation::LevelStart)
		, PreferredPlayDestination(::Durin::Editor::EPlayDestination::EmbeddedViewport)
	{
		ViewportClient = std::make_unique<FLevelEditorViewportClient>();
		ViewportToolbar = std::make_unique<FViewportToolbar>();
		ViewportWidget = std::make_shared<MViewport>();
		SceneViewport = FSceneViewport::CreateOffscreen(ViewportClient.get());
		ViewportWidget->SetDisplaySource(SceneViewport);
		if (GEngine != nullptr)
		{
			GEngine->SetMainSceneViewport(SceneViewport);
		}

		CameraPreviewViewportClient = std::make_unique<FCameraPreviewViewportClient>();
		CameraPreviewViewportWidget = std::make_shared<MViewport>();
		CameraPreviewSceneViewport = FSceneViewport::CreateOffscreen(CameraPreviewViewportClient.get());
		CameraPreviewViewportWidget->SetDisplaySource(CameraPreviewSceneViewport);
		if (GEngine != nullptr) GEngine->RegisterAuxiliarySceneViewport(CameraPreviewSceneViewport);
		RegisterViewportConsoleCommands();
	}

	auto FSceneViewportPanel::GetRenderStatisticsSnapshot() const
		-> FSceneViewportStatisticsSnapshot
	{
		return SceneViewport ? SceneViewport->GetRenderStatisticsSnapshot()
			: FSceneViewportStatisticsSnapshot{};
	}

	auto FSceneViewportPanel::GetRenderGraphSnapshot() const
		-> FSceneViewportRenderGraphSnapshot
	{
		return SceneViewport ? SceneViewport->GetRenderGraphSnapshot()
			: FSceneViewportRenderGraphSnapshot{};
	}

	auto FSceneViewportPanel::RequestRenderGraphCapture() -> void
	{
		if (SceneViewport) SceneViewport->RequestRenderGraphCapture();
	}

	FSceneViewportPanel::~FSceneViewportPanel()
	{
		for (const FConsoleCommandHandle Handle : ViewportConsoleCommandHandles)
			FConsoleCommandRegistry::Get().UnregisterCommand(Handle);
		if (GEngine != nullptr) GEngine->UnregisterAuxiliarySceneViewport(CameraPreviewSceneViewport.get());
		CameraPreviewSceneViewport.reset();
		if (GEngine != nullptr) GEngine->SetMainSceneViewport(nullptr);
		SceneViewport.reset();
	}

	auto FSceneViewportPanel::RegisterViewportConsoleCommands() -> void
	{
		auto RegisterCommand = [this](FConsoleCommandDesc Desc) {
			if (const FConsoleCommandHandle Handle = FConsoleCommandRegistry::Get().RegisterCommand(
				std::move(Desc), OwnerGate))
				ViewportConsoleCommandHandles.push_back(Handle);
		};
		RegisterCommand({"viewport.camera.speed", "Gets or sets the editor viewport fly speed.", "viewport.camera.speed [unitsPerSecond]", [this](std::span<const std::string> Args) {
			if (ViewportClient == nullptr || Args.size() > 1) return FConsoleCommandResult::Failure("Usage: viewport.camera.speed [unitsPerSecond]");
			if (!Args.empty())
			{
				float Speed = 0.0f;
				if (!TryParseFloat(Args[0], Speed) || Speed <= 0.0f) return FConsoleCommandResult::Failure("Fly speed must be a positive finite number.");
				ViewportClient->SetMovementSpeed(Speed);
			}
			return FConsoleCommandResult::Success(std::format("Editor viewport fly speed: {:.2f} units/s", ViewportClient->GetMovementSpeed()));
		}});
		RegisterCommand({"viewport.camera.move", "Moves the editor camera along its local forward, right, and up axes.", "viewport.camera.move <forward> [right] [up]", [this](std::span<const std::string> Args) {
			if (ViewportClient == nullptr || Args.empty() || Args.size() > 3) return FConsoleCommandResult::Failure("Usage: viewport.camera.move <forward> [right] [up]");
			FVector3 Delta(0.0);
			for (size_t Index = 0; Index < Args.size(); ++Index)
			{
				float Value = 0.0f;
				if (!TryParseFloat(Args[Index], Value)) return FConsoleCommandResult::Failure("Camera movement values must be finite numbers.");
				Delta[Index] = static_cast<FReal>(Value);
			}
			ViewportClient->MoveCameraLocal(Delta);
			const FVector3& Location = ViewportClient->GetCameraTransform().GetLocation();
			return FConsoleCommandResult::Success(std::format("Editor camera position: {:.3f} {:.3f} {:.3f}", Location.x, Location.y, Location.z));
		}});
		RegisterCommand({"viewport.camera.position", "Gets or sets the editor camera world position.", "viewport.camera.position [x y z]", [this](std::span<const std::string> Args) {
			if (ViewportClient == nullptr || (!Args.empty() && Args.size() != 3)) return FConsoleCommandResult::Failure("Usage: viewport.camera.position [x y z]");
			if (!Args.empty())
			{
				FVector3 Location(0.0);
				for (size_t Index = 0; Index < Args.size(); ++Index)
				{
					float Value = 0.0f;
					if (!TryParseFloat(Args[Index], Value)) return FConsoleCommandResult::Failure("Camera position values must be finite numbers.");
					Location[Index] = static_cast<FReal>(Value);
				}
				ViewportClient->SetCameraLocation(Location);
				if (SceneViewport) SceneViewport->RequestHistoryReset();
			}
			const FVector3& Location = ViewportClient->GetCameraTransform().GetLocation();
			return FConsoleCommandResult::Success(std::format("Editor camera position: {:.3f} {:.3f} {:.3f}", Location.x, Location.y, Location.z));
		}});
		RegisterCommand({"viewport.specular-aa", "Gets or sets Specular AA for the editor viewport.", "viewport.specular-aa [on|off|toggle]", [this](std::span<const std::string> Args) {
			if (ViewportClient == nullptr || Args.size() > 1)
				return FConsoleCommandResult::Failure("Usage: viewport.specular-aa [on|off|toggle]");
			FSceneViewSettings Settings = ViewportClient->GetViewSettings();
			if (!Args.empty())
			{
				if (Args[0] == "on") Settings.Mode.bEnableSpecularAA = true;
				else if (Args[0] == "off") Settings.Mode.bEnableSpecularAA = false;
				else if (Args[0] == "toggle") Settings.Mode.bEnableSpecularAA = !Settings.Mode.bEnableSpecularAA;
				else return FConsoleCommandResult::Failure("Usage: viewport.specular-aa [on|off|toggle]");
				ViewportClient->SetViewSettings(Settings);
			}
			return FConsoleCommandResult::Success(std::format(
				"Editor viewport Specular AA: {}", Settings.Mode.bEnableSpecularAA ? "enabled" : "disabled"));
		}});
	}

	auto FSceneViewportPanel::CaptureCameraState(DLevel* Level, FLevelViewportCameraState& OutState) const -> bool
	{
		if (ViewportClient == nullptr || Level == nullptr || ViewportClient->GetCurrentLevel() != Level) return false;
		OutState = ViewportClient->GetCameraTransform().GetState();
		OutState.NearClip = ViewportClient->GetNearClip();
		OutState.FarClip = ViewportClient->GetFarClip();
		OutState.ViewFadeStart = ViewportClient->GetViewFadeStart();
		OutState.ViewRenderDistance = ViewportClient->GetViewRenderDistance();
		return true;
	}

	auto FSceneViewportPanel::RestoreCameraState(DLevel* Level, const FLevelViewportCameraState* State) -> void
	{
		if (ViewportClient != nullptr) ViewportClient->InitializeForLevel(Level, State);
		if (SceneViewport) SceneViewport->RequestHistoryReset();
	}

	auto FSceneViewportPanel::FinalizeViewportFrame(FLevelEditorContext& Context) -> void
	{
		if (!IsOpen() || ViewportClient == nullptr || ViewportWidget == nullptr || Context.Level == nullptr || Context.bReadOnly) return;
		uint32 Width = 0;
		uint32 Height = 0;
		if (!FLevelEditorViewportClient::ResolveViewportExtent(ViewportWidget->GetDesiredSize(), Width, Height)) return;
		if (ViewportClient->GetCurrentLevel() != Context.Level && SceneViewport)
			SceneViewport->RequestHistoryReset();
		ViewportClient->SetSelectedActors(Context.GetSelectedActors(), Context.GetPrimarySelectedActor());
		ViewportClient->PrepareSceneView(Context.Level, Width, Height);
	}

	auto FSceneViewportPanel::SetPreferredPlayMode(::Durin::Editor::EPlayStartLocation StartLocation, ::Durin::Editor::EPlayDestination Destination) -> void
	{
		PreferredPlayStartLocation = StartLocation;
		PreferredPlayDestination = Destination;
	}

	auto FSceneViewportPanel::GetTransformGizmo() -> FTransformGizmo*
	{
		return ViewportClient ? &ViewportClient->GetTransformGizmo() : nullptr;
	}

	auto FSceneViewportPanel::GetTransformGizmo() const -> const FTransformGizmo*
	{
		return ViewportClient ? &ViewportClient->GetTransformGizmo() : nullptr;
	}

	auto FSceneViewportPanel::IsGridVisible() const -> bool
	{
		return ViewportClient != nullptr && ViewportClient->IsGridVisible();
	}

	auto FSceneViewportPanel::SetGridVisible(bool bVisible) -> void
	{
		if (ViewportClient != nullptr) ViewportClient->SetGridVisible(bVisible);
	}

	auto FSceneViewportPanel::GetCameraMovementSpeed() const -> float
	{
		return ViewportClient != nullptr ? ViewportClient->GetMovementSpeed() : 5.0f;
	}

	auto FSceneViewportPanel::SetCameraMovementSpeed(float Speed) -> void
	{
		if (ViewportClient != nullptr) ViewportClient->SetMovementSpeed(Speed);
	}

	auto FSceneViewportPanel::FocusActor(const AActor* Actor) -> void
	{
		if (ViewportClient != nullptr) ViewportClient->FocusActor(Actor);
		if (Actor != nullptr && SceneViewport)
			SceneViewport->RequestHistoryReset();
	}

	auto FSceneViewportPanel::Draw(FLevelEditorContext& Context) -> void
	{
		if (ViewportClient) ViewportClient->SetPickingSceneIndex(Context.GetPickingSceneIndex());
		Context.ActivateViewportEditMode = [this, &Context](std::string_view Id) { return EditModeManager.Activate(Id, Context); };
		const bool bPlayingInNewWindow = GEditor && GEditor->IsPlayingInNewWindow();
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		const bool bPanelVisible = ::Durin::Editor::WorkspaceUI::BeginDockablePanel(
			Workspace::Type,
			"Scene Viewport",
			"SceneViewport",
			GetOpenPtr(),
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
		);
		ImGui::PopStyleVar();
		if (!bPanelVisible)
		{
			if (GEditor && !bPlayingInNewWindow) GEditor->UpdateEmbeddedPlayMouseTarget(nullptr, false, false);
			if (ViewportClient != nullptr) ViewportClient->ResetNavigation();
			if (CameraPreviewViewportClient != nullptr) CameraPreviewViewportClient->SetCamera(nullptr);
			bViewportHovered = false;
			bViewportFocused = false;
			ImGui::End();
			return;
		}

		if (Context.Level == nullptr)
		{
			if (GEditor && !bPlayingInNewWindow) GEditor->UpdateEmbeddedPlayMouseTarget(nullptr, false, false);
			if (ViewportClient != nullptr) ViewportClient->ResetNavigation();
			if (CameraPreviewViewportClient != nullptr) CameraPreviewViewportClient->SetCamera(nullptr);
			bViewportHovered = false;
			bViewportFocused = false;
			ImGui::TextDisabled("No level is open. Open a Level asset from Content Browser.");
			ImGui::End();
			return;
		}
		UpdateViewportSize();
		if (ViewportClient != nullptr) ViewportClient->SetSelectedActors(Context.GetSelectedActors(), Context.GetPrimarySelectedActor());
		UpdateCameraPreview(Context);
		if (ViewportWidget != nullptr)
		{
			ViewportWidget->Draw();
			if (ViewportWidget->WasTextureDrawn())
			{
				const ImVec2 VpMin = ImGui::GetItemRectMin();
				const ImVec2 VpMax = ImGui::GetItemRectMax();
				const bool bViewportImageHovered = ImGui::IsItemHovered();
				const bool bViewportImageHoveredAllowBlocked =
					ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
				const FSceneViewportStatisticsSnapshot StatisticsSnapshot = SceneViewport
					? SceneViewport->GetRenderStatisticsSnapshot()
					: FSceneViewportStatisticsSnapshot{};
				const FViewportStatisticsOverlayLayout InitialStatisticsLayout =
					CalculateViewportStatisticsOverlayLayout(
						VpMin, VpMax, bShowStatistics,
						GetStableEditorFramesPerSecond());
				const bool bStatisticsInitiallyHovered =
					InitialStatisticsLayout.Contains(ImGui::GetMousePos());
				if (!Context.bReadOnly && !bStatisticsInitiallyHovered
					&& ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload* Payload = ImGui::AcceptDragDropPayload(::Durin::Editor::AssetDragDropPayloadType); Payload && Payload->IsDelivery() && Payload->DataSize == sizeof(::Durin::Editor::FAssetDragDropPayload))
					{
						const auto* AssetPayload = static_cast<const ::Durin::Editor::FAssetDragDropPayload*>(Payload->Data);
						FObjectPath AssetPath;
						DObject* Asset = nullptr;
						AActor* Actor = nullptr;
						if (!FObjectPath::TryCreate(AssetPayload->AssetPath.data(), AssetPath))
							Context.SetError("Dropped asset path is invalid.");
						else if (const Asset::FAssetResult Result = Asset::LoadObject(AssetPath, Asset); !Result)
							Context.SetError(Result.Message);
						else if (DStaticMesh* StaticMesh = Cast<DStaticMesh>(Asset))
						{
							FTransform PlacementTransform;
							FSceneView View;
							uint32 Width = 0;
							uint32 Height = 0;
							FLevelEditorViewportClient::ResolveViewportExtent({VpMax.x - VpMin.x, VpMax.y - VpMin.y}, Width, Height);
							ViewportClient->BuildViewMatrices(Width, Height, View);
							const ImVec2 Mouse = ImGui::GetMousePos();
							FVector3 Origin, Direction;
							if (SceneViewProjection::BuildViewportRay(View, {Mouse.x - VpMin.x, Mouse.y - VpMin.y}, Origin, Direction))
								PlacementTransform.Translation = Origin + Direction * 5.0;

							auto Request = FStaticMeshLevelMutations::CaptureTarget(*Context.Level);
							Request.bReadOnly = Context.bReadOnly;
							Request.Description = "Place static mesh actor";
							Request.Mutations.push_back({
								.Kind = EStaticMeshLevelMutationKind::Create,
								.TargetName = MakeUniqueActorName(*Context.Level, FName(AssetPath.GetAssetPath().GetAssetName())),
								.Desired = {.StaticMesh = StaticMesh, .Transform = PlacementTransform},
							});
							const FStaticMeshLevelMutationPlan Plan = FStaticMeshLevelMutations::Plan(Request);
							const FStaticMeshLevelMutationResult ApplyResult = FStaticMeshLevelMutations::Execute(Plan, {
								.OpenLevel = Context.Level,
								.Transactions = GEditor ? GEditor->GetTransactor() : nullptr,
								.bReadOnly = Context.bReadOnly,
							});
							if (!ApplyResult)
								Context.SetError(ApplyResult.Diagnostic.Message);
							else if (!ApplyResult.ResultActorNames.empty())
								Actor = Context.Level->FindActorByName(ApplyResult.ResultActorNames.front());
						}
						else if (DSkeletalMesh* SkeletalMesh = Cast<DSkeletalMesh>(Asset))
						{
							auto* SkeletalMeshActor = Context.Level->SpawnActor<ASkeletalMeshActor>(FName(AssetPath.GetAssetPath().GetAssetName()));
							if (SkeletalMeshActor)
							{
								std::string BindError;
								if (SkeletalMeshActor->GetSkeletalMeshComponent()->SetSkeletalMesh(SkeletalMesh, BindError))
									Actor = SkeletalMeshActor;
								else
								{
									Context.Level->DestroyActor(SkeletalMeshActor);
									Context.SetError(std::move(BindError));
								}
							}
						}
						else if (DTerrainHeightmap* Heightmap = Cast<DTerrainHeightmap>(Asset))
						{
							FTransform PlacementTransform;
							FSceneView View;
							uint32 Width = 0;
							uint32 Height = 0;
							FLevelEditorViewportClient::ResolveViewportExtent(
								{VpMax.x - VpMin.x, VpMax.y - VpMin.y}, Width, Height);
							ViewportClient->BuildViewMatrices(Width, Height, View);
							const ImVec2 Mouse = ImGui::GetMousePos();
							FVector3 Origin, Direction;
							if (SceneViewProjection::BuildViewportRay(View,
								{Mouse.x - VpMin.x, Mouse.y - VpMin.y}, Origin, Direction))
								PlacementTransform.Translation = Origin + Direction * 5.0;
							auto Request = FTerrainPlacement::CaptureTarget(*Context.Level);
							Request.bReadOnly = Context.bReadOnly;
							Request.ActorName = MakeUniqueActorName(*Context.Level, FName(AssetPath.GetAssetPath().GetAssetName()));
							Request.Heightmap = Heightmap;
							Request.ExpectedHeightmapRevision = Heightmap->GetRevision();
							Request.Transform = PlacementTransform;
							const FTerrainPlacementResult Result = FTerrainPlacement::Execute(
								FTerrainPlacement::Plan(Request), {
									.OpenLevel = Context.Level,
									.Transactions = GEditor ? GEditor->GetTransactor() : nullptr,
									.bReadOnly = Context.bReadOnly});
							if (!Result) Context.SetError(Result.Diagnostic.Message);
							else Actor = Result.Actor.Get();
						}
						else if (DTextureCube* TextureCube = Cast<DTextureCube>(Asset))
						{
							const FSkyBoxPlacementResult Result = FSkyBoxPlacement::PlaceTextureCube(
								*Context.Level,
								TextureCube,
								FName(std::format("{}_SkyBox", AssetPath.GetAssetPath().GetAssetName())),
								GEditor ? GEditor->GetTransactor() : nullptr,
								Context.bReadOnly);
							if (!Result)
								Context.SetError(Result.Message);
							else
								Actor = Result.Actor;
						}
						else
							Context.SetError("Only StaticMesh, SkeletalMesh, TerrainHeightmap, and TextureCube assets can be placed in the scene viewport.");
						if (Actor)
						{
							FSceneView View;
							uint32 Width = 0;
							uint32 Height = 0;
							FLevelEditorViewportClient::ResolveViewportExtent({VpMax.x - VpMin.x, VpMax.y - VpMin.y}, Width, Height);
							ViewportClient->BuildViewMatrices(Width, Height, View);
							const ImVec2 Mouse = ImGui::GetMousePos();
							FVector3 Origin, Direction;
							if (!Actor->IsA<AStaticMeshActor>() && !Actor->IsA<ASkyBoxActor>()
								&& !Actor->IsA<ATerrainActor>()
								&& SceneViewProjection::BuildViewportRay(View, {Mouse.x - VpMin.x, Mouse.y - VpMin.y}, Origin, Direction)
								&& Actor->GetRootComponent())
								Actor->GetRootComponent()->SetWorldLocation(Origin + Direction * 5.0);
							if (!Actor->IsA<AStaticMeshActor>() && !Actor->IsA<ASkyBoxActor>()
								&& !Actor->IsA<ATerrainActor>()) Context.InvalidatePackageSavedState(Actor->GetPackage());
							Context.SelectActor(Actor);
						}
					}
					ImGui::EndDragDropTarget();
				}
				EditModeManager.Synchronize(Context);
				FViewportClient* RenderSettingsClient = bPlayingInNewWindow && GEditor
					? GEditor->GetPlayViewportRenderSettingsClient()
					: ViewportClient.get();
				const FViewportToolbarLayout ToolbarLayout = ViewportToolbar->CalculateLayout(
					RenderSettingsClient, bPlayingInNewWindow,
					&EditModeManager, VpMin, VpMax);
				const FViewportStatisticsOverlayLayout StatisticsLayout =
					CalculateViewportStatisticsOverlayLayout(
						VpMin, VpMax, bShowStatistics,
						GetStableEditorFramesPerSecond());
				const bool bStatisticsHovered =
					StatisticsLayout.Contains(ImGui::GetMousePos());
				bViewportHovered = bViewportImageHovered && !bStatisticsHovered;
				const bool bToolbarHovered = ImGui::IsMouseHoveringRect(ToolbarLayout.BackgroundMin, ToolbarLayout.BackgroundMax)
					|| ImGui::IsMouseHoveringRect(ToolbarLayout.PlayBackgroundMin, ToolbarLayout.PlayBackgroundMax)
					|| bStatisticsHovered;
				const bool bPopupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
				const bool bRightMousePressed = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
				const bool bNavigationMousePressed = bRightMousePressed || ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || (ImGui::GetIO().KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left));
				const bool bPopupDismissRightPressHovered = bRightMousePressed
					&& ImGui::GetTopMostPopupModal() == nullptr
					&& bViewportImageHoveredAllowBlocked
					&& !bStatisticsHovered;
				if ((bViewportHovered && bNavigationMousePressed) || bPopupDismissRightPressHovered)
				{
					ImGui::SetWindowFocus();
					bViewportHovered = true;
				}
				bViewportFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
				if (GEditor && Context.bReadOnly && !bPlayingInNewWindow)
				{
					std::shared_ptr<FGenericWindow> HostWindow;
					FGenericWindow* HostWindowRaw = static_cast<FGenericWindow*>(ImGui::GetWindowViewport()->PlatformHandle);
					for (const std::shared_ptr<MWindow>& Window : Mona::FMonaApplication::Get().GetWindows())
					{
						if (Window && Window->GetNativeWindow().get() == HostWindowRaw)
						{
							HostWindow = Window->GetNativeWindow();
							break;
						}
					}
					const bool bCaptureClicked = bViewportHovered && !bToolbarHovered && !bPopupOpen
						&& ImGui::IsMouseClicked(ImGuiMouseButton_Left);
					GEditor->UpdateEmbeddedPlayMouseTarget(HostWindow, bViewportFocused, bCaptureClicked);
				}
				if (Context.bReadOnly)
				{
					if (ViewportClient) ViewportClient->ResetNavigation();
					ViewportClient->SetSelectedActors({}, nullptr);
				}
				else UpdateViewportInput(Context, ToolbarLayout);
				if (GEditor && GEditor->IsPlaying())
				{
					DrawViewportPlayStateBorder(VpMin, VpMax, GEditor->IsPlaySessionPaused());
				}
				ViewportToolbar->Draw(
					Context, ViewportClient.get(), RenderSettingsClient, &EditModeManager,
					PreferredPlayStartLocation, PreferredPlayDestination, ToolbarLayout);
				DrawCameraPreview(VpMin, VpMax);
				if (ShouldDrawViewportOrientationOverlay(
					GEditor && GEditor->IsPlaying(), bPlayingInNewWindow))
				{
					DrawViewportOrientationOverlay(ViewportClient.get(), VpMin, VpMax);
				}
				DrawViewportCameraSpeedOverlay(ViewportClient.get(), VpMin, VpMax);
				bool bOpenDetails = false;
				DrawViewportStatisticsOverlay(
					VpMin, VpMax, StatisticsSnapshot, bShowStatistics,
					&bOpenDetails);
				if (bOpenDetails && OpenRenderingDiagnostics)
					OpenRenderingDiagnostics();
				if (Context.bReadOnly)
				{
					ImDrawList* DrawList = ImGui::GetWindowDrawList();
					const char* Status = bPlayingInNewWindow ? "PLAYING IN NEW WINDOW"
						: GEditor && GEditor->IsPlaySessionPaused() ? "PLAY PAUSED"
						: GEditor && !GEditor->IsPlayMouseCaptured() ? "CLICK TO CAPTURE MOUSE"
						: "PLAYING";
					const ImVec2 TextSize = ImGui::CalcTextSize(Status);
					const ImVec2 Padding(MonaImGui::ScaleUI(8.0f), MonaImGui::ScaleUI(4.0f));
					const ImVec2 BadgeSize(TextSize.x + Padding.x * 2.0f, TextSize.y + Padding.y * 2.0f);
					const ImVec2 BadgeMin((VpMin.x + VpMax.x - BadgeSize.x) * 0.5f, VpMax.y - BadgeSize.y - MonaImGui::ScaleUI(10.0f));
					const ImVec2 BadgeMax = Add(BadgeMin, BadgeSize);
					ImVec4 BadgeColor = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
					BadgeColor.w = 0.88f;
					DrawList->AddRectFilled(BadgeMin, BadgeMax, ImGui::GetColorU32(BadgeColor), BadgeMax.y - BadgeMin.y);
					DrawList->AddRect(BadgeMin, BadgeMax, ImGui::GetColorU32(ImGuiCol_CheckMark), BadgeMax.y - BadgeMin.y);
					DrawList->AddText(Add(BadgeMin, Padding), ImGui::GetColorU32(ImGuiCol_CheckMark), Status);
				}
			}
		}
		if (ViewportWidget == nullptr || !ViewportWidget->WasTextureDrawn())
		{
			if (GEditor && !bPlayingInNewWindow) GEditor->UpdateEmbeddedPlayMouseTarget(nullptr, false, false);
			if (ViewportClient != nullptr) ViewportClient->ResetNavigation();
			bViewportHovered = false;
			bViewportFocused = false;
			ImGui::TextDisabled("Viewport initializing...");
		}
		ImGui::End();
	}


	auto FSceneViewportPanel::UpdateCameraPreview(FLevelEditorContext& Context) -> void
	{
		DCameraComponent* Camera = nullptr;
		if (!Context.bReadOnly && !(GEditor && GEditor->IsPlaying()))
		{
			if (AActor* SelectedActor = Context.GetPrimarySelectedActor())
			{
				Camera = SelectedActor->FindComponentByClass<DCameraComponent>();
			}
		}
		if (CameraPreviewViewportClient != nullptr)
		{
			if (CameraPreviewViewportClient->GetCamera() != Camera
				&& CameraPreviewSceneViewport)
				CameraPreviewSceneViewport->RequestHistoryReset();
			CameraPreviewViewportClient->SetCamera(Camera);
		}
	}

	auto FSceneViewportPanel::DrawCameraPreview(const ImVec2& ViewportMin, const ImVec2& ViewportMax) -> void
	{
		if (CameraPreviewViewportClient == nullptr || CameraPreviewViewportClient->GetCamera() == nullptr || CameraPreviewViewportWidget == nullptr) return;

		const float ViewportWidth = ViewportMax.x - ViewportMin.x;
		const float ViewportHeight = ViewportMax.y - ViewportMin.y;
		const float Padding = MonaImGui::ScaleUI(12.0f);
		const float HeaderHeight = MonaImGui::ScaleUI(24.0f);
		const DCameraComponent* Camera = CameraPreviewViewportClient->GetCamera();
		const float MainViewportAspectRatio = ViewportHeight > 0.0f ? ViewportWidth / ViewportHeight : 16.0f / 9.0f;
		const float PreviewAspectRatio = Camera->ResolveAspectRatio(MainViewportAspectRatio);
		const float PreviewWidth = FMath::Clamp(ViewportWidth * 0.28f, MonaImGui::ScaleUI(220.0f), MonaImGui::ScaleUI(360.0f));
		const float PreviewHeight = PreviewWidth / PreviewAspectRatio;
		if (PreviewWidth + Padding * 2.0f > ViewportWidth || PreviewHeight + HeaderHeight + Padding * 2.0f > ViewportHeight) return;

		const ImVec2 ImageMin(ViewportMax.x - Padding - PreviewWidth, ViewportMax.y - Padding - PreviewHeight);
		const ImVec2 ImageMax(ImageMin.x + PreviewWidth, ImageMin.y + PreviewHeight);
		const ImVec2 HeaderMin(ImageMin.x, ImageMin.y - HeaderHeight);
		const ImVec2 HeaderMax(ImageMax.x, ImageMin.y);
		const ImVec2 SavedCursor = ImGui::GetCursorScreenPos();
		CameraPreviewViewportWidget->SetDesiredSize({PreviewWidth, PreviewHeight});
		ImGui::SetCursorScreenPos(ImageMin);
		ImGui::PushClipRect(ViewportMin, ViewportMax, true);
		CameraPreviewViewportWidget->Draw();
		ImGui::PopClipRect();
		ImGui::SetCursorScreenPos(SavedCursor);

		ImDrawList* DrawList = ImGui::GetWindowDrawList();
		ImVec4 HeaderColor = ImGui::GetStyleColorVec4(ImGuiCol_PopupBg);
		HeaderColor.w = 0.94f;
		DrawList->AddRectFilled(HeaderMin, HeaderMax, ImGui::GetColorU32(HeaderColor), MonaImGui::ScaleUI(5.0f), ImDrawFlags_RoundCornersTop);
		DrawList->AddRect(HeaderMin, ImageMax, ImGui::GetColorU32(ImGuiCol_Border), MonaImGui::ScaleUI(5.0f));
		const char* Label = "Camera Preview";
		const ImVec2 LabelSize = ImGui::CalcTextSize(Label);
		DrawList->AddText(ImVec2(HeaderMin.x + MonaImGui::ScaleUI(8.0f), HeaderMin.y + (HeaderHeight - LabelSize.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), Label);
	}


	auto FSceneViewportPanel::UpdateViewportInput(FLevelEditorContext& Context, const FViewportToolbarLayout& ToolbarLayout) -> void
	{
		if (ViewportClient == nullptr) return;
		const ImGuiIO& IO = ImGui::GetIO();
		FLevelEditorViewportInput Input;
		Input.DeltaSeconds = IO.DeltaTime;
		Input.MouseDelta = {IO.MouseDelta.x, IO.MouseDelta.y};
		Input.MouseWheel = IO.MouseWheel;
		Input.bHovered = bViewportHovered;
		Input.bFocused = bViewportFocused;
		Input.bWantTextInput = IO.WantTextInput;
		Input.bAlt = IO.KeyAlt;
		Input.bShift = IO.KeyShift;
		Input.bCtrl = IO.KeyCtrl;
		Input.bLeftMouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
		Input.bMiddleMouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
		Input.bRightMouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Right);
		Input.bLeftMousePressed = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
		Input.bLeftMouseDoubleClicked = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
		Input.bMiddleMousePressed = ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
		Input.bRightMousePressed = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
		Input.bMoveForward = ImGui::IsKeyDown(ImGuiKey_W);
		Input.bMoveBackward = ImGui::IsKeyDown(ImGuiKey_S);
		Input.bMoveLeft = ImGui::IsKeyDown(ImGuiKey_A);
		Input.bMoveRight = ImGui::IsKeyDown(ImGuiKey_D);
		Input.bMoveDown = ImGui::IsKeyDown(ImGuiKey_Q);
		Input.bMoveUp = ImGui::IsKeyDown(ImGuiKey_E);
		Input.bFocusSelection = ImGui::IsKeyPressed(ImGuiKey_F, false);
		Input.bCancel = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
		const bool bGizmoShortcutAllowed = Input.bHovered && Input.bFocused && !Input.bWantTextInput && !Input.bRightMouseDown && !Input.bMiddleMouseDown && !Input.bAlt;
		Input.bModeTranslate = bGizmoShortcutAllowed && ImGui::IsKeyPressed(ImGuiKey_W, false);
		Input.bModeRotate = bGizmoShortcutAllowed && ImGui::IsKeyPressed(ImGuiKey_E, false);
		Input.bModeScale = bGizmoShortcutAllowed && ImGui::IsKeyPressed(ImGuiKey_R, false);
		Input.bDelete = bGizmoShortcutAllowed && ImGui::IsKeyPressed(ImGuiKey_Delete, false);
		Input.bDuplicate = bGizmoShortcutAllowed && Input.bCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false);
		Input.bAppend = bGizmoShortcutAllowed && ImGui::IsKeyPressed(ImGuiKey_Insert, false);
		const ImVec2 ViewportMin = ImGui::GetItemRectMin();
		const ImVec2 ViewportMax = ImGui::GetItemRectMax();
		const ImVec2 MousePosition = ImGui::GetMousePos();
		Input.MousePosition = {MousePosition.x - ViewportMin.x, MousePosition.y - ViewportMin.y};
		Input.ViewportSize = {ViewportMax.x - ViewportMin.x, ViewportMax.y - ViewportMin.y};
		const bool bToolbarHovered = ImGui::IsMouseHoveringRect(ToolbarLayout.BackgroundMin, ToolbarLayout.BackgroundMax)
			|| ImGui::IsMouseHoveringRect(ToolbarLayout.PlayBackgroundMin, ToolbarLayout.PlayBackgroundMax);
		const bool bPopupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
		if (bToolbarHovered || bPopupOpen) Input.bLeftMousePressed = false;
		::Durin::DTransactor* Transactions = GEditor != nullptr ? GEditor->GetTransactor() : nullptr;
		ViewportClient->Update(Context.Level, Context.GetPrimarySelectedActor(), Input);
		if (Input.bFocusSelection && !Input.bWantTextInput
			&& Context.GetPrimarySelectedActor() != nullptr && SceneViewport)
			SceneViewport->RequestHistoryReset();
		uint32 ViewWidth = 0;
		uint32 ViewHeight = 0;
		FSceneView SceneView;
		if (!FLevelEditorViewportClient::ResolveViewportExtent(Input.ViewportSize, ViewWidth, ViewHeight)
			|| !ViewportClient->BuildViewMatrices(ViewWidth, ViewHeight, SceneView)) return;
		if (!ViewportClient->GetTransformGizmo().IsDragging() && Input.bHovered)
			ViewportClient->UpdateHoveredVisualizationWithView(Context.Level, SceneView, Input.MousePosition);
		else if (!Input.bHovered)
			ViewportClient->UpdateHoveredVisualization(nullptr, {}, {});
		const bool bGizmoConsumesMouse = ViewportClient->GetTransformGizmo().IsHovered() || ViewportClient->GetTransformGizmo().IsDragging();
		Input.bRequestSelection = Input.bHovered && Input.bLeftMousePressed && !Input.bAlt && !Input.bWantTextInput && !bToolbarHovered && !bGizmoConsumesMouse && !bPopupOpen;
		if (Input.bRequestSelection) ImGui::SetWindowFocus();
		EditModeManager.Tick(Context, *ViewportClient, SceneView, Input, Transactions);
		if (ViewportClient->GetTransformGizmo().IsDragging())
		{
			Input.bLeftMousePressed = false;
			Input.bMiddleMousePressed = false;
			Input.bRightMousePressed = false;
			Input.bMiddleMouseDown = false;
			Input.bRightMouseDown = false;
			Input.MouseWheel = 0.0f;
		}
		ViewportClient->SetSelectedActors(Context.GetSelectedActors(), Context.GetPrimarySelectedActor());
		ViewportClient->SetSelectedComponent(Context.GetSelectedComponent(), Context.GetSelectedSubElements());
	}

	auto FSceneViewportPanel::UpdateViewportSize() -> void
	{
		ImVec2 AvailableSize = ImGui::GetContentRegionAvail();
		AvailableSize.x = FMath::Max(8.0f, AvailableSize.x);
		AvailableSize.y = FMath::Max(8.0f, AvailableSize.y);
		if (ViewportWidget != nullptr)
		{
			ViewportWidget->SetDesiredSize({AvailableSize.x, AvailableSize.y});
		}
	}
} // namespace Durin::Editor::Level
