#include "Panels/SceneViewportPanel.h"

#include "AssetSystem.h"
#include "Components/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Assets/ContentBrowserDragDrop.h"
#include "Editor/EditorEngine.h"
#include "Editor/EditorTransaction.h"
#include "Editor/EditorWorkspaceUI.h"
#include "Engine/Engine.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Actors/StaticMeshActor.h"
#include "Workspace/LevelEditorContext.h"
#include "Workspace/LevelEditorWorkspace.h"
#include "Math/Vector.h"
#include "Mona/SceneViewport.h"
#include "MonaImGui.h"
#include "SceneViewProjection.h"
#include "Viewport/LevelEditorViewportClient.h"
#include "Viewport/CameraPreviewViewportClient.h"
#include "Viewport/ViewportPresentation.h"
#include "Widgets/MViewport.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	namespace
	{
		auto Add(const ImVec2& A, const ImVec2& B) -> ImVec2 { return ImVec2(A.x + B.x, A.y + B.y); }
	} // namespace


	FSceneViewportPanel::FSceneViewportPanel()
		: PreferredPlayStartLocation(EEditorPlayStartLocation::LevelStart)
		, PreferredPlayDestination(EEditorPlayDestination::EmbeddedViewport)
	{
		ViewportClient = std::make_unique<FLevelEditorViewportClient>();
		ViewportToolbar = std::make_unique<FViewportToolbar>();
		ViewportWidget = std::make_shared<MViewport>();
		const std::shared_ptr<FSceneViewport> SceneViewport = std::make_shared<FSceneViewport>(ViewportClient.get(), ViewportWidget);
		ViewportWidget->SetViewportInterface(SceneViewport);
		if (GEngine != nullptr)
		{
			GEngine->SetMainSceneViewport(SceneViewport);
		}

		CameraPreviewViewportClient = std::make_unique<FCameraPreviewViewportClient>();
		CameraPreviewViewportWidget = std::make_shared<MViewport>();
		CameraPreviewSceneViewport = std::make_shared<FSceneViewport>(CameraPreviewViewportClient.get(), CameraPreviewViewportWidget);
		CameraPreviewViewportWidget->SetViewportInterface(CameraPreviewSceneViewport);
		if (GEngine != nullptr) GEngine->RegisterAuxiliarySceneViewport(CameraPreviewSceneViewport);
	}

	FSceneViewportPanel::~FSceneViewportPanel()
	{
		if (GEngine != nullptr) GEngine->UnregisterAuxiliarySceneViewport(CameraPreviewSceneViewport.get());
		CameraPreviewSceneViewport.reset();
		if (GEngine != nullptr) GEngine->SetMainSceneViewport(nullptr);
	}

	auto FSceneViewportPanel::CaptureCameraState(DLevel* Level, FLevelViewportCameraState& OutState) const -> bool
	{
		if (ViewportClient == nullptr || Level == nullptr || ViewportClient->GetCurrentLevel() != Level) return false;
		OutState = ViewportClient->GetCameraTransform().GetState();
		return true;
	}

	auto FSceneViewportPanel::RestoreCameraState(DLevel* Level, const FLevelViewportCameraState* State) -> void
	{
		if (ViewportClient != nullptr) ViewportClient->InitializeForLevel(Level, State);
	}

	auto FSceneViewportPanel::FinalizeViewportFrame(FLevelEditorContext& Context) -> void
	{
		if (!IsOpen() || ViewportClient == nullptr || ViewportWidget == nullptr || Context.Level == nullptr || Context.bReadOnly) return;
		uint32 Width = 0;
		uint32 Height = 0;
		if (!FLevelEditorViewportClient::ResolveViewportExtent(ViewportWidget->GetDesiredSize(), Width, Height)) return;
		ViewportClient->SetSelectedActors(Context.GetSelectedActors(), Context.GetPrimarySelectedActor());
		ViewportClient->PrepareSceneView(Context.Level, Width, Height);
	}

	auto FSceneViewportPanel::SetPreferredPlayMode(EEditorPlayStartLocation StartLocation, EEditorPlayDestination Destination) -> void
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

	auto FSceneViewportPanel::FocusActor(const AActor* Actor) -> void
	{
		if (ViewportClient != nullptr) ViewportClient->FocusActor(Actor);
	}

	auto FSceneViewportPanel::Draw(FLevelEditorContext& Context) -> void
	{
		Context.ActivateViewportEditMode = [this, &Context](std::string_view Id) { return EditModeManager.Activate(Id, Context); };
		const bool bPlayingInNewWindow = GEditor && GEditor->IsPlayingInNewWindow();
		if (!EditorWorkspaceUI::BeginDockablePanel(
			LevelEditorWorkspace::Type,
			"Scene Viewport",
			"SceneViewport",
			GetOpenPtr(),
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
		))
		{
			if (GEngine && !bPlayingInNewWindow) GEngine->SetGameInputEnabled(false);
			if (ViewportClient != nullptr) ViewportClient->ResetNavigation();
			if (CameraPreviewViewportClient != nullptr) CameraPreviewViewportClient->SetCamera(nullptr);
			bViewportHovered = false;
			bViewportFocused = false;
			ImGui::End();
			return;
		}

		if (Context.Level == nullptr)
		{
			if (GEngine && !bPlayingInNewWindow) GEngine->SetGameInputEnabled(false);
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
				if (!Context.bReadOnly && ImGui::BeginDragDropTarget())
				{
					if (const ImGuiPayload* Payload = ImGui::AcceptDragDropPayload(ContentBrowserAssetPayloadType); Payload && Payload->IsDelivery() && Payload->DataSize == sizeof(FContentBrowserAssetPayload))
					{
						const auto* AssetPayload = static_cast<const FContentBrowserAssetPayload*>(Payload->Data);
						FAssetPath AssetPath;
						DStaticMesh* Mesh = nullptr;
						if (!FAssetPath::TryCreate(AssetPayload->AssetPath.data(), AssetPath))
							Context.SetError("Dropped asset path is invalid.");
						else if (const Asset::FAssetResult Result = Asset::LoadAsset(AssetPath, Mesh); !Result)
							Context.SetError(Result.Message);
						else if (AStaticMeshActor* Actor = Context.Level->SpawnActor<AStaticMeshActor>(FName(AssetPath.GetAssetName())))
						{
							Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
							FSceneView View;
							uint32 Width = 0;
							uint32 Height = 0;
							FLevelEditorViewportClient::ResolveViewportExtent({VpMax.x - VpMin.x, VpMax.y - VpMin.y}, Width, Height);
							ViewportClient->BuildViewMatrices(Width, Height, View);
							const ImVec2 Mouse = ImGui::GetMousePos();
							FVector3 Origin, Direction;
							if (SceneViewProjection::BuildViewportRay(View, {Mouse.x - VpMin.x, Mouse.y - VpMin.y}, Origin, Direction) && Actor->GetRootComponent())
								Actor->GetRootComponent()->SetWorldLocation(Origin + Direction * 5.0);
							Context.InvalidatePackageSavedState(Actor->GetPackage());
							Context.SelectActor(Actor);
						}
					}
					ImGui::EndDragDropTarget();
				}
				EditModeManager.Synchronize(Context);
				const FViewportToolbarLayout ToolbarLayout = ViewportToolbar->CalculateLayout(ViewportClient.get(), &EditModeManager, VpMin, VpMax);
				bViewportHovered = ImGui::IsItemHovered();
				const bool bRightMousePressed = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
				const bool bNavigationMousePressed = bRightMousePressed || ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || (ImGui::GetIO().KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left));
				const bool bPopupDismissRightPressHovered = bRightMousePressed
					&& ImGui::GetTopMostPopupModal() == nullptr
					&& ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
				if ((bViewportHovered && bNavigationMousePressed) || bPopupDismissRightPressHovered)
				{
					ImGui::SetWindowFocus();
					bViewportHovered = true;
				}
				bViewportFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
				if (GEngine && !bPlayingInNewWindow) GEngine->SetGameInputEnabled(Context.bReadOnly && bViewportFocused);
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
				ViewportToolbar->Draw(Context, ViewportClient.get(), &EditModeManager, PreferredPlayStartLocation, PreferredPlayDestination, ToolbarLayout);
				DrawCameraPreview(VpMin, VpMax);
				DrawViewportOrientationOverlay(ViewportClient.get(), VpMin, VpMax);
				DrawViewportFPSOverlay(VpMin, VpMax);
				if (Context.bReadOnly)
				{
					ImDrawList* DrawList = ImGui::GetWindowDrawList();
					const char* Status = bPlayingInNewWindow ? "PLAYING IN NEW WINDOW" : GEditor && GEditor->IsPlaySessionPaused() ? "PLAY PAUSED" : "PLAYING";
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
		if (CameraPreviewViewportClient != nullptr) CameraPreviewViewportClient->SetCamera(Camera);
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
		FEditorTransactionManager* Transactions = GEditor != nullptr ? &GEditor->GetTransactionManager() : nullptr;
		ViewportClient->Update(Context.Level, Context.GetPrimarySelectedActor(), Input);
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
} // namespace Durin
