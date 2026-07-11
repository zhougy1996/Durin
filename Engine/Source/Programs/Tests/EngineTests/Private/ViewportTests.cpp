#include "Actors/CameraActor.h"
#include "Client/ViewportClient.h"
#include "Components/CameraComponent.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "IRendererModule.h"
#include "Mona/SceneViewport.h"
#include "Viewport/ViewportCameraTransform.h"

#include <gtest/gtest.h>

namespace
{
	class FTestViewportClient final : public Durin::FViewportClient
	{
	public:
		auto CalcSceneView(Durin::uint32 Width, Durin::uint32 Height, Durin::FSceneView& OutView) const -> bool override
		{
			OutView.ViewportWidth = Width;
			OutView.ViewportHeight = Height;
			OutView.ViewLocation = {11.0, 12.0, 13.0};
			return true;
		}
	};

	class FTestEngine final : public Durin::DEngine
	{
	public:
		FTestEngine() : DEngine(Durin::FObjectInitializer::Get()) {}
		using DEngine::BuildMainSceneView;

		auto SetTestWorld(Durin::DWorld* World) -> void { MainWorld = World; }
		auto SetTestViewport(const std::shared_ptr<Durin::FSceneViewport>& Viewport) -> void { MainSceneViewport = Viewport; }
	};

	auto ExpectVectorNear(const Durin::FVector3& Actual, const Durin::FVector3& Expected, double Tolerance = 1.e-6) -> void
	{
		EXPECT_NEAR(Actual.x, Expected.x, Tolerance);
		EXPECT_NEAR(Actual.y, Expected.y, Tolerance);
		EXPECT_NEAR(Actual.z, Expected.z, Tolerance);
	}

	auto InitializeDObjectSystem() -> void
	{
		static const bool bInitialized = []() {
			Durin::GGameThreadId = Durin::FPlatformLTS::GetCurrentThreadId();
			Durin::GIsGameThreadIdInitialized = true;
			Durin::DObjectInit();
			return true;
		}();
		(void)bInitialized;
	}
}

TEST(FViewportCameraTransformTests, ClampsPitchAndBuildsOrthonormalDirections)
{
	Durin::FViewportCameraTransform Camera;
	Camera.Rotate(0.0f, 200.0f);
	EXPECT_DOUBLE_EQ(Camera.GetPitch(), 89.0);
	EXPECT_NEAR(glm::length(Camera.GetForwardVector()), 1.0, 1.e-8);
	EXPECT_NEAR(glm::dot(Camera.GetForwardVector(), Camera.GetRightVector()), 0.0, 1.e-8);
	EXPECT_NEAR(glm::dot(Camera.GetForwardVector(), Camera.GetUpVector()), 0.0, 1.e-8);
}

TEST(FViewportCameraTransformTests, MovesPansAndPreservesOrbitDistance)
{
	Durin::FViewportCameraTransform Camera;
	const Durin::FVector3 InitialLocation = Camera.GetLocation();
	const Durin::FVector3 InitialPivot = Camera.GetOrbitPivot();
	Camera.MoveLocal({2.0, 0.0, 0.0});
	ExpectVectorNear(Camera.GetOrbitPivot() - InitialPivot, Camera.GetLocation() - InitialLocation);

	Camera.Pan(1.0f, -0.5f);
	const double Distance = Camera.GetOrbitDistance();
	Camera.Orbit(35.0f, 15.0f);
	EXPECT_NEAR(glm::length(Camera.GetOrbitPivot() - Camera.GetLocation()), Distance, 1.e-8);
}

TEST(FViewportCameraTransformTests, FocusAndDollyRemainFiniteAtDegenerateDistance)
{
	Durin::FViewportCameraTransform Camera;
	Camera.Focus({3.0, 4.0, 5.0}, 0.0f);
	EXPECT_GE(Camera.GetOrbitDistance(), 0.05);
	ExpectVectorNear(Camera.GetOrbitPivot(), {3.0, 4.0, 5.0});
	Camera.Dolly(100000.0f);
	EXPECT_GE(Camera.GetOrbitDistance(), 0.05);
	const Durin::FVector3 Location = Camera.GetLocation();
	EXPECT_TRUE(std::isfinite(Location.x) && std::isfinite(Location.y) && std::isfinite(Location.z));
}

TEST(FViewportSelectionTests, PrefersViewportClientAndFallsBackToPrimaryCamera)
{
	std::cerr << "viewport step 1\n";
	InitializeDObjectSystem();
	FTestEngine Engine;
	std::cerr << "viewport step 2\n";
	FTestViewportClient Client;
	auto ClientViewport = std::make_shared<Durin::FSceneViewport>(&Client, std::shared_ptr<Durin::MViewport>{});
	Engine.SetTestViewport(ClientViewport);
	ExpectVectorNear(Engine.BuildMainSceneView(640, 480).ViewLocation, {11.0, 12.0, 13.0});
	std::cerr << "viewport step 3\n";

	Durin::DWorld* World = Durin::NewObject<Durin::DWorld>(&Engine, "ViewportTestWorld");
	Durin::DLevel* Level = Durin::NewObject<Durin::DLevel>(World, "ViewportTestLevel");
	ASSERT_TRUE(World->SetCurrentLevel(Level));
	std::cerr << "viewport step 4\n";
	Durin::ACameraActor* CameraActor = Level->SpawnActor<Durin::ACameraActor>("Camera");
	ASSERT_NE(CameraActor, nullptr);
	std::cerr << "viewport step 5\n";
	CameraActor->GetCameraComponent()->SetWorldLocation({7.0, 8.0, 9.0});
	Engine.SetTestWorld(World);
	Engine.SetTestViewport(nullptr);
	ExpectVectorNear(Engine.BuildMainSceneView(640, 480).ViewLocation, {7.0, 8.0, 9.0});
	std::cerr << "viewport step 6\n";
}
