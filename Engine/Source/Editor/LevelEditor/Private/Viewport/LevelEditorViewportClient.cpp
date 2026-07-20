#include "Viewport/LevelEditorViewportClient.h"
#include "MonaImGui.h"

#include "Editor/EditorEngine.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "IRendererModule.h"
#include "SceneViewProjection.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin
{
	namespace
	{
		constexpr float kLookSensitivity = 0.15f;
		constexpr float kOrbitSensitivity = 0.2f;
		constexpr float kPanSensitivity = 0.01f;
		constexpr float kSpeedWheelScale = 1.2f;
		constexpr float kMinMovementSpeed = 0.05f;
		constexpr float kMaxMovementSpeed = 10000.0f;
		constexpr float kShiftSpeedMultiplier = 4.0f;
		constexpr float kFocusDistance = 5.0f;
		constexpr double kIntersectionEpsilon = 1.e-8;

		auto IntersectRayBox(const FVector3& Origin, const FVector3& Direction, const FBox& Box) -> bool
		{
			if (!Box.bIsValid) return false;
			double TMin = 0.0;
			double TMax = std::numeric_limits<double>::max();
			for (uint32 Axis = 0; Axis < 3; ++Axis)
			{
				if (std::abs(Direction[Axis]) <= kIntersectionEpsilon)
				{
					if (Origin[Axis] < Box.Min[Axis] || Origin[Axis] > Box.Max[Axis]) return false;
					continue;
				}
				double Near = (Box.Min[Axis] - Origin[Axis]) / Direction[Axis];
				double Far = (Box.Max[Axis] - Origin[Axis]) / Direction[Axis];
				if (Near > Far) std::swap(Near, Far);
				TMin = std::max(TMin, Near);
				TMax = std::min(TMax, Far);
				if (TMin > TMax) return false;
			}
			return TMax >= 0.0;
		}

		auto IntersectRayTriangle(const FVector3& Origin, const FVector3& Direction, const FVector3& A, const FVector3& B, const FVector3& C, double& OutDistance) -> bool
		{
			const FVector3 Edge1 = B - A;
			const FVector3 Edge2 = C - A;
			const FVector3 P = glm::cross(Direction, Edge2);
			const double Determinant = glm::dot(Edge1, P);
			if (std::abs(Determinant) <= kIntersectionEpsilon) return false;
			const double InvDeterminant = 1.0 / Determinant;
			const FVector3 T = Origin - A;
			const double U = glm::dot(T, P) * InvDeterminant;
			if (U < -kIntersectionEpsilon || U > 1.0 + kIntersectionEpsilon) return false;
			const FVector3 Q = glm::cross(T, Edge1);
			const double V = glm::dot(Direction, Q) * InvDeterminant;
			if (V < -kIntersectionEpsilon || U + V > 1.0 + kIntersectionEpsilon) return false;
			OutDistance = glm::dot(Edge2, Q) * InvDeterminant;
			return OutDistance >= 0.0;
		}
	} // namespace

	auto FLevelEditorViewportClient::CalcSceneView(uint32 Width, uint32 Height, FSceneView& OutView) const -> bool
	{
		if (GEditor && GEditor->IsPlaying()) return false;
		const float AspectRatio = Height > 0 ? static_cast<float>(Width) / static_cast<float>(Height) : 1.0f;
		const float HalfFovRadians = glm::radians(FieldOfViewDegrees) * 0.5f;
		const float YScale = 1.0f / std::tan(HalfFovRadians);
		const float XScale = YScale / std::max(AspectRatio, 0.001f);
		const float DepthScale = FarClip / (FarClip - NearClip);
		const float DepthBias = -NearClip * FarClip / (FarClip - NearClip);

		OutView.ViewportWidth = Width;
		OutView.ViewportHeight = Height;
		OutView.ViewMatrix = CameraTransform.GetViewMatrix();
		OutView.ProjectionMatrix = FMatrix(0.0f);
		OutView.ProjectionMatrix[1][0] = XScale;
		OutView.ProjectionMatrix[2][1] = -YScale;
		OutView.ProjectionMatrix[0][2] = DepthScale;
		OutView.ProjectionMatrix[3][2] = DepthBias;
		OutView.ProjectionMatrix[0][3] = 1.0f;
		OutView.ViewProjectionMatrix = OutView.ProjectionMatrix * OutView.ViewMatrix;
		OutView.ViewLocation = CameraTransform.GetLocation();
		const ImVec4& GridColor = MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::ViewportText);
		const ImVec4& AxisXColor = MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::AxisX);
		const ImVec4& AxisYColor = MonaImGui::GetThemeColor(MonaImGui::EUIThemeColor::AxisY);
		OutView.EditorGrid = {
			.bVisible = bShowGrid,
			.Height = 0.0,
			.FadeDistance = FarClip * 0.95f,
			.MinorColor = {GridColor.x, GridColor.y, GridColor.z, GridColor.w * 0.14f},
			.MajorColor = {GridColor.x, GridColor.y, GridColor.z, GridColor.w * 0.32f},
			.AxisXColor = {AxisXColor.x, AxisXColor.y, AxisXColor.z, AxisXColor.w * 0.82f},
			.AxisYColor = {AxisYColor.x, AxisYColor.y, AxisYColor.z, AxisYColor.w * 0.82f},
		};
		AppendSelectionBounds(OutView);
		FEditorVisualizationCollector VisualizationCollector;
		CollectEditorVisualizations(CurrentLevel, OutView, VisualizationCollector);
		VisualizationCollector.AppendToView(OutView);
		TransformGizmo.AppendOverlayPrimitives(OutView);
		return true;
	}

	auto FLevelEditorViewportClient::CollectEditorVisualizations(DLevel* Level, const FSceneView& View, FEditorVisualizationCollector& Collector) const -> void
	{
		if (!Level) return;
		const auto& Registry = FLevelEditorCustomizationRegistry::Get();
		for (const TObjectPtr<AActor>& ActorPtr : Level->GetActors())
		{
			AActor* Actor = ActorPtr.Get();
			if (!Actor) continue;
			for (const TObjectPtr<DActorComponent>& ComponentPtr : Actor->GetOwnedComponents())
			{
				DActorComponent* Component = ComponentPtr.Get();
				if (!Component) continue;
				const std::shared_ptr<IComponentEditorVisualizer> Visualizer = Registry.FindComponentVisualizer(Component->GetClass());
				if (!Visualizer) continue;
				const FEditorVisualizationContext Context{
					.View = View,
					.Level = Level,
					.bSelected = std::ranges::any_of(SelectedActors, [Actor](const TObjectPtr<AActor>& Entry) { return Entry.Get() == Actor; }),
					.bHovered = HoveredVisualizationActor.Get() == Actor,
					.bPrimarySelection = PrimarySelectedActor.Get() == Actor,
				};
				Visualizer->DrawVisualization(Component, Context, Collector);
			}
		}
	}

	auto FLevelEditorViewportClient::SetSelectedActors(const std::vector<TObjectPtr<AActor>>& Actors, AActor* PrimaryActor) -> void
	{
		SelectedActors = Actors;
		PrimarySelectedActor = PrimaryActor;
	}

	auto FLevelEditorViewportClient::FocusActor(const AActor* Actor) -> void
	{
		if (Actor == nullptr) return;
		if (const DSceneComponent* RootComponent = Actor->GetRootComponent())
		{
			CameraTransform.Focus(RootComponent->GetWorldLocation(), kFocusDistance);
		}
	}

	auto FLevelEditorViewportClient::AppendSelectionBounds(FSceneView& View) const -> void
	{
		for (const TObjectPtr<AActor>& ActorPtr : SelectedActors)
		{
			const AActor* Actor = ActorPtr.Get();
			if (Actor == nullptr) continue;
			const bool bPrimary = Actor == PrimarySelectedActor.Get();
			const ImVec4& ThemeColor = MonaImGui::GetThemeColor(bPrimary ? MonaImGui::EUIThemeColor::SelectionPrimary : MonaImGui::EUIThemeColor::SelectionSecondary);
			const FVector4f Color{ThemeColor.x, ThemeColor.y, ThemeColor.z, ThemeColor.w};
			for (const TObjectPtr<DActorComponent>& ComponentPtr : Actor->GetOwnedComponents())
			{
				const auto* Component = Cast<DStaticMeshComponent>(ComponentPtr.Get());
				const DStaticMesh* Mesh = Component != nullptr ? Component->GetStaticMesh() : nullptr;
				const FStaticMeshRenderData* Data = Mesh != nullptr ? Mesh->GetRenderData() : nullptr;
				if (Data == nullptr || !Data->LocalBounds.bIsValid) continue;
				const FVector3 Center = Data->LocalBounds.GetCenter();
				const FVector3 Size = Data->LocalBounds.Max - Data->LocalBounds.Min;
				const FMatrix BoundsToLocal = glm::translate(FMatrix(1.0), Center) * glm::scale(FMatrix(1.0), Size);
				View.OverlayPrimitives.push_back({EViewOverlayShape::WireBox, Component->GetRenderMatrix() * BoundsToLocal, Color});
			}
		}
	}

	auto FLevelEditorViewportClient::Update(DLevel* Level, AActor* SelectedActor, const FLevelEditorViewportInput& Input) -> void
	{
		if (Level != CurrentLevel) InitializeForLevel(Level);
		if (!Input.bFocused)
		{
			ResetNavigation();
			return;
		}

		if (Input.bRightMousePressed && Input.bHovered) bFlyNavigation = true;
		if (Input.bLeftMousePressed && Input.bHovered && Input.bAlt) bOrbitNavigation = true;
		if (Input.bMiddleMousePressed && Input.bHovered) bPanNavigation = true;
		if (!Input.bRightMouseDown) bFlyNavigation = false;
		if (!Input.bLeftMouseDown || !Input.bAlt) bOrbitNavigation = false;
		if (!Input.bMiddleMouseDown) bPanNavigation = false;

		if (bFlyNavigation)
		{
			CameraTransform.Rotate(Input.MouseDelta.x * kLookSensitivity, -Input.MouseDelta.y * kLookSensitivity);
			if (Input.MouseWheel != 0.0f)
			{
				MovementSpeed = std::clamp(MovementSpeed * std::pow(kSpeedWheelScale, Input.MouseWheel), kMinMovementSpeed, kMaxMovementSpeed);
			}
			if (!Input.bWantTextInput)
			{
				FVector3 Direction(0.0f);
				Direction.x = static_cast<float>(Input.bMoveForward) - static_cast<float>(Input.bMoveBackward);
				Direction.y = static_cast<float>(Input.bMoveRight) - static_cast<float>(Input.bMoveLeft);
				Direction.z = static_cast<float>(Input.bMoveUp) - static_cast<float>(Input.bMoveDown);
				if (glm::dot(Direction, Direction) > 0.0f)
				{
					Direction = glm::normalize(Direction);
					const float Speed = MovementSpeed * (Input.bShift ? kShiftSpeedMultiplier : 1.0f);
					CameraTransform.MoveLocal(Direction * static_cast<FReal>(Speed * Input.DeltaSeconds));
				}
			}
		}
		else if (bOrbitNavigation)
		{
			CameraTransform.Orbit(Input.MouseDelta.x * kOrbitSensitivity, -Input.MouseDelta.y * kOrbitSensitivity);
		}
		else if (bPanNavigation)
		{
			const float Scale = static_cast<float>(std::max(1.0, CameraTransform.GetOrbitDistance())) * kPanSensitivity;
			CameraTransform.Pan(-Input.MouseDelta.x * Scale, Input.MouseDelta.y * Scale);
		}
		else if (Input.bHovered && Input.MouseWheel != 0.0f)
		{
			CameraTransform.Dolly(Input.MouseWheel * static_cast<float>(std::max(0.25, CameraTransform.GetOrbitDistance() * 0.15)));
		}

		if (!Input.bWantTextInput && Input.bFocusSelection) FocusActor(SelectedActor);
	}

	auto FLevelEditorViewportClient::BuildPickingRay(const FVector2f& ViewportPosition, const FVector2f& ViewportSize, FVector3& OutOrigin, FVector3& OutDirection) const -> bool
	{
		if (ViewportSize.x <= 0.0f || ViewportSize.y <= 0.0f) return false;
		FSceneView View;
		if (!CalcSceneView(static_cast<uint32>(ViewportSize.x), static_cast<uint32>(ViewportSize.y), View)) return false;
		return SceneViewProjection::BuildViewportRay(View, ViewportPosition, OutOrigin, OutDirection);
	}

	auto FLevelEditorViewportClient::PickActor(DLevel* Level, const FVector2f& ViewportPosition, const FVector2f& ViewportSize) const -> AActor*
	{
		if (Level == nullptr || ViewportSize.x <= 0.0f || ViewportSize.y <= 0.0f) return nullptr;
		FSceneView View;
		if (!CalcSceneView(static_cast<uint32>(ViewportSize.x), static_cast<uint32>(ViewportSize.y), View)) return nullptr;
		FVector3 RayOrigin;
		FVector3 RayDirection;
		if (!SceneViewProjection::BuildViewportRay(View, ViewportPosition, RayOrigin, RayDirection)) return nullptr;
		AActor* ClosestActor = nullptr;
		double ClosestDistance = std::numeric_limits<double>::max();
		for (const TObjectPtr<AActor>& ActorPtr : Level->GetActors())
		{
			AActor* Actor = ActorPtr.Get();
			if (Actor == nullptr) continue;
			for (const TObjectPtr<DActorComponent>& ComponentPtr : Actor->GetOwnedComponents())
			{
				auto* Component = Cast<DStaticMeshComponent>(ComponentPtr.Get());
				const DStaticMesh* Mesh = Component != nullptr ? Component->GetStaticMesh() : nullptr;
				const FStaticMeshRenderData* Data = Mesh != nullptr ? Mesh->GetRenderData() : nullptr;
				if (Data == nullptr || !Data->LocalBounds.bIsValid || Data->LODResources.empty()) continue;
				const FStaticMeshLODResources& LOD = Data->LODResources[0];
				if (LOD.Indices.size() < 3) continue;
				const FMatrix LocalToWorld = Component->GetRenderMatrix();
				const double Determinant = glm::determinant(LocalToWorld);
				if (!std::isfinite(Determinant) || std::abs(Determinant) <= kIntersectionEpsilon) continue;
				const FMatrix WorldToLocal = glm::inverse(LocalToWorld);
				const FVector3 LocalOrigin = FVector3(WorldToLocal * FVector4(RayOrigin, 1.0));
				const FVector3 LocalDirection = FVector3(WorldToLocal * FVector4(RayDirection, 0.0));
				if (!IntersectRayBox(LocalOrigin, LocalDirection, Data->LocalBounds)) continue;
				for (size_t Index = 0; Index + 2 < LOD.Indices.size(); Index += 3)
				{
					const uint32 I0 = LOD.Indices[Index];
					const uint32 I1 = LOD.Indices[Index + 1];
					const uint32 I2 = LOD.Indices[Index + 2];
					if (I0 >= LOD.Positions.size() || I1 >= LOD.Positions.size() || I2 >= LOD.Positions.size()) continue;
					double LocalDistance = 0.0;
					if (!IntersectRayTriangle(LocalOrigin, LocalDirection, FVector3(LOD.Positions[I0]), FVector3(LOD.Positions[I1]), FVector3(LOD.Positions[I2]), LocalDistance)) continue;
					const FVector3 LocalHit = LocalOrigin + LocalDirection * LocalDistance;
					const FVector3 WorldHit = FVector3(LocalToWorld * FVector4(LocalHit, 1.0));
					const double WorldDistance = glm::length(WorldHit - RayOrigin);
					if (std::isfinite(WorldDistance) && WorldDistance < ClosestDistance)
					{
						ClosestDistance = WorldDistance;
						ClosestActor = Actor;
					}
				}
			}
		}
		FEditorVisualizationCollector VisualizationCollector;
		CollectEditorVisualizations(Level, View, VisualizationCollector);
		const FEditorVisualizationHit VisualizationHit = VisualizationCollector.HitTest(View, ViewportPosition);
		if (VisualizationHit.Actor && (VisualizationHit.bDepthIndependent || VisualizationHit.Distance < ClosestDistance)) ClosestActor = VisualizationHit.Actor;
		return ClosestActor;
	}

	auto FLevelEditorViewportClient::UpdateHoveredVisualization(DLevel* Level, const FVector2f& ViewportPosition, const FVector2f& ViewportSize) -> void
	{
		HoveredVisualizationActor = nullptr;
		if (!Level || ViewportSize.x <= 0.0f || ViewportSize.y <= 0.0f) return;
		FSceneView View;
		if (!CalcSceneView(static_cast<uint32>(ViewportSize.x), static_cast<uint32>(ViewportSize.y), View)) return;
		FEditorVisualizationCollector Collector;
		CollectEditorVisualizations(Level, View, Collector);
		HoveredVisualizationActor = Collector.HitTest(View, ViewportPosition).Actor;
	}

	auto FLevelEditorViewportClient::ProjectWorldToViewport(const FVector3& WorldPosition, const FVector2f& ViewportSize, FVector2f& OutPosition) const -> bool
	{
		if (ViewportSize.x <= 0.0f || ViewportSize.y <= 0.0f) return false;
		FSceneView View;
		if (!CalcSceneView(static_cast<uint32>(ViewportSize.x), static_cast<uint32>(ViewportSize.y), View)) return false;
		return SceneViewProjection::ProjectWorldToViewport(View, WorldPosition, OutPosition);
	}

	auto FLevelEditorViewportClient::ResetNavigation() -> void
	{
		bFlyNavigation = false;
		bOrbitNavigation = false;
		bPanNavigation = false;
	}

	auto FLevelEditorViewportClient::InitializeForLevel(DLevel* Level, const FLevelViewportCameraState* SavedState) -> void
	{
		CurrentLevel = Level;
		ResetNavigation();
		HoveredVisualizationActor = nullptr;
		if (SavedState != nullptr)
			CameraTransform.SetState(*SavedState);
		else
			CameraTransform.Reset();
	}
} // namespace Durin
