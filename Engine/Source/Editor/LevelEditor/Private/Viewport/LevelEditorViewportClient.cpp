#include "Viewport/LevelEditorViewportClient.h"
#include "MonaImGui.h"

#include "Editor/EditorEngine.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Math/Operations.h"
#include "SceneView.h"
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
		constexpr float kMaxNavigationDeltaSeconds = 1.0f / 30.0f;
		constexpr float kLookSmoothingRate = 30.0f;
		constexpr float kMovementSmoothingRate = 24.0f;
		constexpr double kIntersectionEpsilon = 1.e-8;

		template <typename T>
		auto SmoothVelocityAndIntegrate(T& Velocity, const T& TargetVelocity, float SmoothingRate, float DeltaSeconds) -> T
		{
			if (DeltaSeconds <= 0.0f) return T(0.0);
			const float Decay = std::exp(-SmoothingRate * DeltaSeconds);
			using FScalar = typename T::value_type;
			const FScalar TypedDeltaSeconds = static_cast<FScalar>(DeltaSeconds);
			const FScalar IntegrationScale = static_cast<FScalar>((1.0f - Decay) / SmoothingRate);
			const FScalar TypedDecay = static_cast<FScalar>(Decay);
			const T Difference = Velocity - TargetVelocity;
			const T Integrated = TargetVelocity * TypedDeltaSeconds + Difference * IntegrationScale;
			Velocity = TargetVelocity + Difference * TypedDecay;
			return Integrated;
		}

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
			const FVector3 P = Math::Cross(Direction, Edge2);
			const double Determinant = Math::Dot(Edge1, P);
			if (std::abs(Determinant) <= kIntersectionEpsilon) return false;
			const double InvDeterminant = 1.0 / Determinant;
			const FVector3 T = Origin - A;
			const double U = Math::Dot(T, P) * InvDeterminant;
			if (U < -kIntersectionEpsilon || U > 1.0 + kIntersectionEpsilon) return false;
			const FVector3 Q = Math::Cross(T, Edge1);
			const double V = Math::Dot(Direction, Q) * InvDeterminant;
			if (V < -kIntersectionEpsilon || U + V > 1.0 + kIntersectionEpsilon) return false;
			OutDistance = Math::Dot(Edge2, Q) * InvDeterminant;
			return OutDistance >= 0.0;
		}
	} // namespace

	auto FLevelEditorViewportClient::CalcSceneView(uint32 Width, uint32 Height, FSceneView& OutView) const -> bool
	{
		if (GEditor && GEditor->IsPlaying()) return false;
		if (PreparedSceneView.bReadyForRender && PreparedSceneView.FrameNumber == GFrameCounter
			&& PreparedSceneView.Level.Get() == CurrentLevel && PreparedSceneView.Width == Width && PreparedSceneView.Height == Height)
		{
			OutView = PreparedSceneView.View;
			return true;
		}
		if (!BuildCompleteSceneView(CurrentLevel, Width, Height, PreparedSceneView)) return false;
		PreparedSceneView.bReadyForRender = false;
		OutView = PreparedSceneView.View;
		return true;
	}

	auto FLevelEditorViewportClient::BuildViewMatrices(uint32 Width, uint32 Height, FSceneView& OutView) const -> bool
	{
		if ((GEditor && GEditor->IsPlaying()) || Width == 0 || Height == 0) return false;
		OutView = {};
		const float AspectRatio = Height > 0 ? static_cast<float>(Width) / static_cast<float>(Height) : 1.0f;
		const float HalfFovRadians = Math::DegreesToRadians(FieldOfViewDegrees) * 0.5f;
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
		return true;
	}

	auto FLevelEditorViewportClient::ResolveViewportExtent(const FVector2f& ViewportSize, uint32& OutWidth, uint32& OutHeight) -> bool
	{
		if (ViewportSize.x <= 0.0f || ViewportSize.y <= 0.0f) return false;
		OutWidth = static_cast<uint32>(FMath::Max(8, FMath::CeilToInt(ViewportSize.x)));
		OutHeight = static_cast<uint32>(FMath::Max(8, FMath::CeilToInt(ViewportSize.y)));
		return true;
	}

	auto FLevelEditorViewportClient::BuildCompleteSceneView(DLevel* Level, uint32 Width, uint32 Height, FPreparedSceneView& OutFrame) const -> bool
	{
		FSceneView View;
		if (!BuildViewMatrices(Width, Height, View)) return false;
		FEditorVisualizationCollector Visualizations;
		PopulateEditorOverlays(Level, View, Visualizations);
		AppendSelectionBounds(View);
		Visualizations.AppendToView(View, HoveredVisualization.Actor ? &HoveredVisualization : nullptr);
		TransformGizmo.AppendOverlayPrimitives(View);
		OutFrame.View = std::move(View);
		OutFrame.Visualizations = std::move(Visualizations);
		OutFrame.Level = Level;
		OutFrame.FrameNumber = GFrameCounter;
		OutFrame.Width = Width;
		OutFrame.Height = Height;
		return true;
	}

	auto FLevelEditorViewportClient::PrepareSceneView(DLevel* Level, uint32 Width, uint32 Height) -> void
	{
		if (Level != CurrentLevel) InitializeForLevel(Level);
		if (!BuildCompleteSceneView(Level, Width, Height, PreparedSceneView))
			InvalidatePreparedSceneView();
		else
			PreparedSceneView.bReadyForRender = true;
	}

	auto FLevelEditorViewportClient::PopulateEditorOverlays(DLevel* Level, const FSceneView& View, FEditorVisualizationCollector& Collector) const -> void
	{
		if (!Level) return;
		const auto& Registry = FLevelEditorCustomizationRegistry::Get();
		for (const TObjectPtr<AActor>& ActorPtr : Level->GetActors())
		{
			AActor* Actor = ActorPtr.Get();
			if (!Actor || Actor->IsHidden()) continue;
			const bool bSelected = std::ranges::any_of(SelectedActors, [Actor](const TObjectPtr<AActor>& Entry) { return Entry.Get() == Actor; });
			const bool bPrimarySelection = PrimarySelectedActor.Get() == Actor;
			for (const TObjectPtr<DActorComponent>& ComponentPtr : Actor->GetOwnedComponents())
			{
				DActorComponent* Component = ComponentPtr.Get();
				if (!Component) continue;
				const std::shared_ptr<IComponentEditorVisualizer> Visualizer = Registry.FindComponentVisualizer(Component->GetClass());
				if (!Visualizer) continue;
				const FEditorVisualizationContext Context{
					.View = View,
					.Level = Level,
					.bSelected = bSelected,
					.bPrimarySelection = bPrimarySelection,
					.bComponentSelected = SelectedComponent.Get() == Component,
					.SelectedSubElements = SelectedComponent.Get() == Component ? std::span<const FEditorSubElementSelection>(SelectedSubElements) : std::span<const FEditorSubElementSelection>{},
				};
				Visualizer->DrawVisualization(Component, Context, Collector);
			}
		}
	}

	auto FLevelEditorViewportClient::SetSelectedComponent(DActorComponent* Component, const std::vector<FEditorSubElementSelection>& Elements) -> void
	{
		SelectedComponent = Component;
		SelectedSubElements = Elements;
		InvalidatePreparedSceneView();
	}

	auto FLevelEditorViewportClient::SetSelectedActors(const std::vector<TObjectPtr<AActor>>& Actors, AActor* PrimaryActor) -> void
	{
		SelectedActors = Actors;
		PrimarySelectedActor = PrimaryActor;
		InvalidatePreparedSceneView();
	}

	auto FLevelEditorViewportClient::FocusActor(const AActor* Actor) -> void
	{
		if (Actor == nullptr) return;
		if (const DSceneComponent* RootComponent = Actor->GetRootComponent())
		{
			CameraTransform.Focus(RootComponent->GetWorldLocation(), kFocusDistance);
			InvalidatePreparedSceneView();
		}
	}

	auto FLevelEditorViewportClient::FocusLocation(const FVector3& WorldLocation) -> void
	{
		CameraTransform.Focus(WorldLocation, kFocusDistance);
		InvalidatePreparedSceneView();
	}

	auto FLevelEditorViewportClient::SetGridVisible(bool bVisible) -> void
	{
		if (bShowGrid == bVisible) return;
		bShowGrid = bVisible;
		InvalidatePreparedSceneView();
	}

	auto FLevelEditorViewportClient::AppendSelectionBounds(FSceneView& View) const -> void
	{
		for (const TObjectPtr<AActor>& ActorPtr : SelectedActors)
		{
			const AActor* Actor = ActorPtr.Get();
			if (Actor == nullptr || Actor->IsHidden()) continue;
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
				const FMatrix BoundsToLocal = Math::TranslationMatrix(Center) * Math::ScaleMatrix(Size);
				View.OverlayPrimitives.push_back({EViewOverlayShape::WireBox, Component->GetRenderMatrix() * BoundsToLocal, Color});
			}
		}
	}

	auto FLevelEditorViewportClient::Update(DLevel* Level, AActor* SelectedActor, const FLevelEditorViewportInput& Input) -> void
	{
		if (Level != CurrentLevel) InitializeForLevel(Level);
		InvalidatePreparedSceneView();
		if (!Input.bFocused)
		{
			ResetNavigation();
			return;
		}

		if (Input.bRightMousePressed && Input.bHovered) bFlyNavigation = true;
		if (Input.bLeftMousePressed && Input.bHovered && Input.bAlt) bOrbitNavigation = true;
		if (Input.bMiddleMousePressed && Input.bHovered) bPanNavigation = true;
		if (!Input.bRightMouseDown)
		{
			bFlyNavigation = false;
			ResetFlyMotion();
		}
		if (!Input.bLeftMouseDown || !Input.bAlt) bOrbitNavigation = false;
		if (!Input.bMiddleMouseDown) bPanNavigation = false;

		if (bFlyNavigation)
		{
			const float DeltaSeconds = std::clamp(Input.DeltaSeconds, 0.0f, kMaxNavigationDeltaSeconds);
			const FVector2f TargetLookVelocity = DeltaSeconds > 0.0f
				? FVector2f(Input.MouseDelta.x * kLookSensitivity, -Input.MouseDelta.y * kLookSensitivity) / DeltaSeconds
				: FVector2f(0.0f);
			const FVector2f LookDelta = SmoothVelocityAndIntegrate(FlyLookVelocity, TargetLookVelocity, kLookSmoothingRate, DeltaSeconds);
			if (Input.MouseWheel != 0.0f)
			{
				MovementSpeed = std::clamp(MovementSpeed * std::pow(kSpeedWheelScale, Input.MouseWheel), kMinMovementSpeed, kMaxMovementSpeed);
			}
			FVector3 TargetMovementVelocity(0.0);
			if (!Input.bWantTextInput)
			{
				FVector3 Direction(0.0f);
				Direction.x = static_cast<float>(Input.bMoveForward) - static_cast<float>(Input.bMoveBackward);
				Direction.y = static_cast<float>(Input.bMoveRight) - static_cast<float>(Input.bMoveLeft);
				Direction.z = static_cast<float>(Input.bMoveUp) - static_cast<float>(Input.bMoveDown);
				if (Math::LengthSquared(Direction) > 0.0f)
				{
					Direction = Math::Normalize(Direction);
					const float Speed = MovementSpeed * (Input.bShift ? kShiftSpeedMultiplier : 1.0f);
					TargetMovementVelocity = Direction * static_cast<FReal>(Speed);
				}
			}
			const FVector3 MovementDelta = SmoothVelocityAndIntegrate(
				FlyMovementVelocity,
				TargetMovementVelocity,
				kMovementSmoothingRate,
				DeltaSeconds
			);
			CameraTransform.Rotate(LookDelta.x * 0.5f, LookDelta.y * 0.5f);
			CameraTransform.MoveLocal(MovementDelta);
			CameraTransform.Rotate(LookDelta.x * 0.5f, LookDelta.y * 0.5f);
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
		uint32 Width = 0;
		uint32 Height = 0;
		if (!ResolveViewportExtent(ViewportSize, Width, Height)) return false;
		FSceneView View;
		if (!BuildViewMatrices(Width, Height, View)) return false;
		return SceneViewProjection::BuildViewportRay(View, ViewportPosition, OutOrigin, OutDirection);
	}

	auto FLevelEditorViewportClient::PickActor(DLevel* Level, const FVector2f& ViewportPosition, const FVector2f& ViewportSize) const -> AActor*
	{
		uint32 Width = 0;
		uint32 Height = 0;
		if (Level == nullptr || !ResolveViewportExtent(ViewportSize, Width, Height)) return nullptr;
		FSceneView View;
		if (!BuildViewMatrices(Width, Height, View)) return nullptr;
		return PickActorWithView(Level, View, ViewportPosition);
	}

	auto FLevelEditorViewportClient::PickActorWithView(DLevel* Level, const FSceneView& View, const FVector2f& ViewportPosition) const -> AActor*
	{
		if (Level == nullptr) return nullptr;
		FVector3 RayOrigin;
		FVector3 RayDirection;
		if (!SceneViewProjection::BuildViewportRay(View, ViewportPosition, RayOrigin, RayDirection)) return nullptr;
		AActor* ClosestActor = nullptr;
		double ClosestDistance = std::numeric_limits<double>::max();
		for (const TObjectPtr<AActor>& ActorPtr : Level->GetActors())
		{
			AActor* Actor = ActorPtr.Get();
			if (Actor == nullptr || Actor->IsHidden()) continue;
			for (const TObjectPtr<DActorComponent>& ComponentPtr : Actor->GetOwnedComponents())
			{
				auto* Component = Cast<DStaticMeshComponent>(ComponentPtr.Get());
				const DStaticMesh* Mesh = Component != nullptr ? Component->GetStaticMesh() : nullptr;
				const FStaticMeshRenderData* Data = Mesh != nullptr ? Mesh->GetRenderData() : nullptr;
				if (Data == nullptr || !Data->LocalBounds.bIsValid || Data->LODResources.empty()) continue;
				const FStaticMeshLODResources& LOD = Data->LODResources[0];
				const auto& Positions =
					LOD.VertexBuffers.PositionVertexBuffer.GetPositions();
				const auto& Indices = LOD.IndexBuffer.GetIndices();
				if (Indices.size() < 3) continue;
				const FMatrix LocalToWorld = Component->GetRenderMatrix();
				const double Determinant = Math::Determinant(LocalToWorld);
				if (!std::isfinite(Determinant) || std::abs(Determinant) <= kIntersectionEpsilon) continue;
				const FMatrix WorldToLocal = Math::Inverse(LocalToWorld);
				const FVector3 LocalOrigin = FVector3(WorldToLocal * FVector4(RayOrigin, 1.0));
				const FVector3 LocalDirection = FVector3(WorldToLocal * FVector4(RayDirection, 0.0));
				if (!IntersectRayBox(LocalOrigin, LocalDirection, Data->LocalBounds)) continue;
				for (size_t Index = 0; Index + 2 < Indices.size(); Index += 3)
				{
					const uint32 I0 = Indices[Index];
					const uint32 I1 = Indices[Index + 1];
					const uint32 I2 = Indices[Index + 2];
					if (I0 >= Positions.size() || I1 >= Positions.size() || I2 >= Positions.size()) continue;
					double LocalDistance = 0.0;
					if (!IntersectRayTriangle(LocalOrigin, LocalDirection, FVector3(Positions[I0]), FVector3(Positions[I1]), FVector3(Positions[I2]), LocalDistance)) continue;
					const FVector3 LocalHit = LocalOrigin + LocalDirection * LocalDistance;
					const FVector3 WorldHit = FVector3(LocalToWorld * FVector4(LocalHit, 1.0));
					const double WorldDistance = Math::Length(WorldHit - RayOrigin);
					if (std::isfinite(WorldDistance) && WorldDistance < ClosestDistance)
					{
						ClosestDistance = WorldDistance;
						ClosestActor = Actor;
					}
				}
			}
		}
		FEditorVisualizationCollector ColdVisualizations;
		const FEditorVisualizationCollector* Visualizations = &PreparedSceneView.Visualizations;
		if (PreparedSceneView.Level.Get() != Level)
		{
			PopulateEditorOverlays(Level, View, ColdVisualizations);
			Visualizations = &ColdVisualizations;
		}
		const FEditorVisualizationHit VisualizationHit = Visualizations->HitTest(View, ViewportPosition);
		if (VisualizationHit.Actor && (VisualizationHit.bDepthIndependent || VisualizationHit.Distance < ClosestDistance)) ClosestActor = VisualizationHit.Actor;
		return ClosestActor;
	}

	auto FLevelEditorViewportClient::PickVisualizationWithView(DLevel* Level, const FSceneView& View, const FVector2f& ViewportPosition) const -> FEditorVisualizationHit
	{
		if (!Level) return {};
		FEditorVisualizationCollector ColdVisualizations;
		const FEditorVisualizationCollector* Visualizations = &PreparedSceneView.Visualizations;
		if (PreparedSceneView.Level.Get() != Level)
		{
			PopulateEditorOverlays(Level, View, ColdVisualizations);
			Visualizations = &ColdVisualizations;
		}
		return Visualizations->HitTest(View, ViewportPosition);
	}

	auto FLevelEditorViewportClient::UpdateHoveredVisualization(DLevel* Level, const FVector2f& ViewportPosition, const FVector2f& ViewportSize) -> void
	{
		uint32 Width = 0;
		uint32 Height = 0;
		if (!Level || !ResolveViewportExtent(ViewportSize, Width, Height))
		{
			UpdateHoveredVisualizationWithView(nullptr, FSceneView{}, {});
			return;
		}
		FSceneView View;
		if (!BuildViewMatrices(Width, Height, View)) return;
		UpdateHoveredVisualizationWithView(Level, View, ViewportPosition);
	}

	auto FLevelEditorViewportClient::UpdateHoveredVisualizationWithView(DLevel* Level, const FSceneView& View, const FVector2f& ViewportPosition) -> void
	{
		HoveredVisualization = {};
		if (Level && PreparedSceneView.Level.Get() == Level)
			HoveredVisualization = PreparedSceneView.Visualizations.HitTest(View, ViewportPosition);
		InvalidatePreparedSceneView();
	}

	auto FLevelEditorViewportClient::ProjectWorldToViewport(const FVector3& WorldPosition, const FVector2f& ViewportSize, FVector2f& OutPosition) const -> bool
	{
		uint32 Width = 0;
		uint32 Height = 0;
		if (!ResolveViewportExtent(ViewportSize, Width, Height)) return false;
		FSceneView View;
		if (!BuildViewMatrices(Width, Height, View)) return false;
		return SceneViewProjection::ProjectWorldToViewport(View, WorldPosition, OutPosition);
	}

	auto FLevelEditorViewportClient::ResetNavigation() -> void
	{
		bFlyNavigation = false;
		bOrbitNavigation = false;
		bPanNavigation = false;
		ResetFlyMotion();
	}

	auto FLevelEditorViewportClient::ResetFlyMotion() -> void
	{
		FlyLookVelocity = FVector2f(0.0f);
		FlyMovementVelocity = FVector3(0.0);
	}

	auto FLevelEditorViewportClient::InitializeForLevel(DLevel* Level, const FLevelViewportCameraState* SavedState) -> void
	{
		CurrentLevel = Level;
		ResetNavigation();
		HoveredVisualization = {};
		InvalidatePreparedSceneView(true);
		if (SavedState != nullptr)
			CameraTransform.SetState(*SavedState);
		else
			CameraTransform.Reset();
	}

	auto FLevelEditorViewportClient::InvalidatePreparedSceneView(bool bDiscardInteractionData) -> void
	{
		PreparedSceneView.bReadyForRender = false;
		if (bDiscardInteractionData) PreparedSceneView = {};
	}
} // namespace Durin
