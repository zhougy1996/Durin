#include "Viewport/LevelEditorViewportClient.h"
#include "Viewport/ViewportPickingService.h"
#include "MonaImGui.h"

#include "Editor/EditorEngine.h"
#include "Collision/CollisionTypes.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Actor.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Math/Operations.h"
#include "SceneView.h"
#include "SceneViewProjection.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"

namespace Durin::Editor::Level
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
		constexpr uint32 kCollisionCircleSegments = 24;
		constexpr FVector4f kCollisionBodyColor{0.1f, 0.85f, 0.35f, 0.9f};
		constexpr FVector4f kCollisionHitColor{1.0f, 0.25f, 0.1f, 1.0f};

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

		auto TransformCollisionPoint(const FMatrix& Transform, const FVector3& Point) -> FVector3
		{
			return FVector3(Transform * FVector4(Point, 1.0));
		}

		auto AddCollisionLine(FSceneView& View, const FMatrix& Transform, const FVector3& Start, const FVector3& End) -> void
		{
			View.OverlayLines.push_back({
				TransformCollisionPoint(Transform, Start),
				TransformCollisionPoint(Transform, End),
				kCollisionBodyColor,
				1.5f});
		}

		template<typename PointFactory>
		auto AddCollisionLoop(FSceneView& View, const FMatrix& Transform, PointFactory&& MakePoint) -> void
		{
			for (uint32 Segment = 0; Segment < kCollisionCircleSegments; ++Segment)
			{
				const double StartAngle = Math::TwoPi<double>() * static_cast<double>(Segment) / kCollisionCircleSegments;
				const double EndAngle = Math::TwoPi<double>() * static_cast<double>(Segment + 1) / kCollisionCircleSegments;
				AddCollisionLine(View, Transform, MakePoint(StartAngle), MakePoint(EndAngle));
			}
		}

		auto AppendCollisionDebug(FSceneView& View, DLevel* Level) -> void
		{
			DWorld* World = Level ? Level->GetWorld() : nullptr;
			if (!World || !World->IsCollisionDebugDrawEnabled()) return;
			const FCollisionDebugSnapshot Snapshot = World->CaptureCollisionDebugSnapshot();
			for (const FCollisionDebugBody& Body : Snapshot.Bodies)
			{
				const FMatrix Transform = Body.Transform.ToMatrix();
				for (const std::array<FVector3, 2>& Bounds : Body.HeightFieldNodeBoundsSample)
				{
					const FVector3 Center = (Bounds[0] + Bounds[1]) * 0.5;
					const FVector3 Size = Bounds[1] - Bounds[0];
					View.OverlayPrimitives.push_back({EViewOverlayShape::WireBox,
						Transform * Math::TranslationMatrix(Center) * Math::ScaleMatrix(Size),
						kCollisionBodyColor});
				}
				for (const std::array<FVector3, 3>& Triangle : Body.TriangleSample)
				{
					AddCollisionLine(View, Transform, Triangle[0], Triangle[1]);
					AddCollisionLine(View, Transform, Triangle[1], Triangle[2]);
					AddCollisionLine(View, Transform, Triangle[2], Triangle[0]);
				}
				if (!Body.bHasPrimitiveShape) continue;
				switch (Body.Shape.GetType())
				{
				case ECollisionShapeType::Box:
					View.OverlayPrimitives.push_back({
						EViewOverlayShape::WireBox,
						Transform * Math::ScaleMatrix(Body.Shape.GetBoxHalfExtent() * 2.0),
						kCollisionBodyColor});
					break;
				case ECollisionShapeType::Sphere:
				{
					const double Radius = Body.Shape.GetSphereRadius();
					AddCollisionLoop(View, Transform, [Radius](double Angle) { return FVector3(std::cos(Angle) * Radius, std::sin(Angle) * Radius, 0.0); });
					AddCollisionLoop(View, Transform, [Radius](double Angle) { return FVector3(std::cos(Angle) * Radius, 0.0, std::sin(Angle) * Radius); });
					AddCollisionLoop(View, Transform, [Radius](double Angle) { return FVector3(0.0, std::cos(Angle) * Radius, std::sin(Angle) * Radius); });
					break;
				}
				case ECollisionShapeType::Capsule:
				{
					const double Radius = Body.Shape.GetCapsuleRadius();
					const double CylinderHalfHeight = Body.Shape.GetCapsuleHalfHeight() - Radius;
					for (double Z : {-CylinderHalfHeight, CylinderHalfHeight})
						AddCollisionLoop(View, Transform, [Radius, Z](double Angle) { return FVector3(std::cos(Angle) * Radius, std::sin(Angle) * Radius, Z); });
					for (uint32 Axis = 0; Axis < 2; ++Axis)
					{
						AddCollisionLoop(View, Transform, [=](double Angle) {
							const double Horizontal = std::cos(Angle) * Radius;
							const double SinAngle = std::sin(Angle);
							const double Z = SinAngle * Radius + (SinAngle >= 0.0 ? CylinderHalfHeight : -CylinderHalfHeight);
							return Axis == 0 ? FVector3(Horizontal, 0.0, Z) : FVector3(0.0, Horizontal, Z);
						});
					}
					break;
				}
				}
			}
			if (Snapshot.LastBlockingHit)
			{
				const FHitResult& Hit = *Snapshot.LastBlockingHit;
				View.OverlayLines.push_back({Hit.ImpactPoint, Hit.ImpactPoint + Hit.ImpactNormal, kCollisionHitColor, 3.0f});
			}
		}

	} // namespace

	FLevelEditorViewportClient::FLevelEditorViewportClient()
		: PickingService(std::make_unique<FViewportPickingService>())
	{
	}

	FLevelEditorViewportClient::~FLevelEditorViewportClient() = default;

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
		AppendCollisionDebug(View, Level);
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
			const FEditorVisualizationContext ActorContext{
				.View = View,
				.Level = Level,
				.bSelected = bSelected,
				.bPrimarySelection = bPrimarySelection,
			};
			if (const std::shared_ptr<IActorEditorVisualizer> Visualizer = Registry.FindActorVisualizer(Actor->GetClass()))
				Visualizer->DrawVisualization(Actor, ActorContext, Collector);
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

	auto FLevelEditorViewportClient::SetMovementSpeed(float Speed) -> void
	{
		if (!std::isfinite(Speed)) return;
		MovementSpeed = std::clamp(Speed, kMinMovementSpeed, kMaxMovementSpeed);
	}

	auto FLevelEditorViewportClient::SetCameraLocation(const FVector3& WorldLocation) -> void
	{
		if (!Math::IsFinite(WorldLocation)) return;
		FLevelViewportCameraState State = CameraTransform.GetState();
		const FVector3 Delta = WorldLocation - State.Location;
		State.Location = WorldLocation;
		State.OrbitPivot += Delta;
		CameraTransform.SetState(State);
		ResetFlyMotion();
		InvalidatePreparedSceneView();
	}

	auto FLevelEditorViewportClient::MoveCameraLocal(const FVector3& LocalDelta) -> void
	{
		if (!Math::IsFinite(LocalDelta)) return;
		CameraTransform.MoveLocal(LocalDelta);
		ResetFlyMotion();
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

	auto FLevelEditorViewportClient::SubmitViewportPick(DLevel* Level, const FSceneView& View,
		const FVector2f& ViewportPosition, EViewportPickLayer Layers) -> FViewportPickSubmission
	{
		if (Level != CurrentLevel) InitializeForLevel(Level);
		std::optional<FViewportPickHit> Visualization;
		if (Level && EnumHasAnyFlags(Layers, EViewportPickLayer::EditorVisualization))
		{
			FEditorVisualizationCollector ColdVisualizations;
			const FEditorVisualizationCollector* Visualizations = &PreparedSceneView.Visualizations;
			if (PreparedSceneView.Level.Get() != Level)
			{
				PopulateEditorOverlays(Level, View, ColdVisualizations);
				Visualizations = &ColdVisualizations;
			}
			const FEditorVisualizationHit Hit = Visualizations->HitTest(View, ViewportPosition);
			if (Hit.Actor && Hit.Component)
			{
				const FObjectHandle Handle = TWeakObjectPtr<DActorComponent>(Hit.Component).GetHandle();
				Visualization = FViewportPickHit{
					.Kind = EViewportPickHitKind::EditorVisualization,
					.Actor = Hit.Actor,
					.Component = Hit.Component,
					.Element = Hit.Element,
					.Distance = Hit.Distance,
					.Priority = Hit.Priority,
					.StableTieKey = (static_cast<uint64>(Handle.Generation) << 32) | Handle.Index,
					.bDepthIndependent = Hit.bDepthIndependent,
				};
			}
		}
		return PickingService->Submit({
			.Level = Level,
			.View = View,
			.ViewportPosition = ViewportPosition,
			.Layers = Layers,
		}, std::move(Visualization));
	}

	auto FLevelEditorViewportClient::PollViewportPick(FViewportPickTicket Ticket) -> FViewportPickCompletion
	{
		return PickingService->Poll(Ticket);
	}

	auto FLevelEditorViewportClient::CancelViewportPick(FViewportPickTicket Ticket) -> void { PickingService->Cancel(Ticket); }
	auto FLevelEditorViewportClient::ReleaseViewportPick(FViewportPickTicket Ticket) -> void { PickingService->Release(Ticket); }
	auto FLevelEditorViewportClient::SetPickingBackendForTesting(std::unique_ptr<IViewportPickingBackend> Backend) -> void
	{
		PickingService->SetBackendForTesting(std::move(Backend));
	}

	auto FLevelEditorViewportClient::SetPickingSceneIndex(std::shared_ptr<FViewportPickingSceneIndex> SceneIndex) -> void
	{
		PickingService->SetSceneIndex(std::move(SceneIndex));
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
		PickingService->SetLevel(Level);
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
} // namespace Durin::Editor::Level
