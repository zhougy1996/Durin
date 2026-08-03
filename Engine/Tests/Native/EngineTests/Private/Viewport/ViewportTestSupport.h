#pragma once

#include "Actors/CameraActor.h"
#include "Actors/DirectionalLightActor.h"
#include "Actors/StaticMeshActor.h"
#include "AssetSystem.h"
#include "Customizations/CameraEditorCustomizations.h"
#include "Client/ViewportClient.h"
#include "Components/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SplineComponent.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Package.h"
#include "DObject/Property.h"
#include "Customizations/DirectionalLightEditorCustomizations.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineTestSupport.h"
#include "Editor/EditorTransaction.h"
#include "SceneView.h"
#include "NativeTestSupport.h"
#include "Settings/LevelViewportSessionSettings.h"
#include "Workspace/LevelEditorContext.h"
#include "LevelEditorCustomizations.h"
#include "MonaImGui.h"
#include "Customizations/ObjectPropertyEditorCustomizations.h"
#include "Mona/SceneViewport.h"
#include "Misc/Paths.h"
#include "SceneViewProjection.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Customizations/SplineEditorCustomizations.h"
#include "Viewport/ViewportCameraTransform.h"
#include "Viewport/LevelEditorViewportClient.h"
#include "Viewport/CameraPreviewViewportClient.h"
#include "Yaml/Yaml.h"

#include <gtest/gtest.h>

namespace
{
	class FCountingTransaction final : public Durin::IEditorTransaction
	{
	public:
		explicit FCountingTransaction(int& InValue, int InDelta = 1) : Value(InValue), Delta(InDelta) {}
		auto GetDescription() const -> std::string_view override { return "Counting"; }
		auto GetDetails(Durin::EEditorTransactionOperation Operation) const -> std::string override
		{
			return Operation == Durin::EEditorTransactionOperation::Undo ? "Counter changed backward" : "Counter changed forward";
		}
		auto Undo() -> bool override { Value -= Delta; return true; }
		auto Redo() -> bool override { Value += Delta; return true; }
	private:
		int& Value;
		int Delta;
	};
	struct FTransactionControl
	{
		bool bFailUndo = false;
		bool bFailRedo = false;
	};
	class FControlledTransaction final : public Durin::IEditorTransaction
	{
	public:
		FControlledTransaction(int& InValue, FTransactionControl& InControl) : Value(InValue), Control(InControl) {}
		auto GetDescription() const -> std::string_view override { return "Controlled"; }
		auto Undo() -> bool override
		{
			if (Control.bFailUndo) return false;
			--Value;
			return true;
		}
		auto Redo() -> bool override
		{
			if (Control.bFailRedo) return false;
			++Value;
			return true;
		}
	private:
		int& Value;
		FTransactionControl& Control;
	};

	class FPackageCountingTransaction final : public Durin::IEditorTransaction
	{
	public:
		FPackageCountingTransaction(
			int& InValue,
			std::initializer_list<Durin::DPackage*> InPackages,
			int InDelta = 1,
			FTransactionControl* InControl = nullptr
		)
			: Value(InValue)
			, Packages(InPackages)
			, Delta(InDelta)
			, Control(InControl)
		{
		}

		auto GetDescription() const -> std::string_view override { return "Package Counting"; }
		auto GetAffectedPackages() const -> std::span<Durin::DPackage* const> override { return Packages; }
		auto Undo() -> bool override
		{
			if (Control && Control->bFailUndo) return false;
			Value -= Delta;
			return true;
		}
		auto Redo() -> bool override
		{
			if (Control && Control->bFailRedo) return false;
			Value += Delta;
			return true;
		}

	private:
		int& Value;
		std::vector<Durin::DPackage*> Packages;
		int Delta;
		FTransactionControl* Control = nullptr;
	};

	auto MakeRevisionTestPackage(std::string_view Label = "Package") -> Durin::DPackage*
	{
		InitializeDObjectSystem();
		Durin::PathUtilities::FScopedMountRegistryFixture MountFixture;
		Durin::PathUtilities::RegisterMountPointForTests(
			"/EditorRevisionTests/",
			Durin::Testing::GetTestWorkDirectory().generic_string() + "/"
		);
		static Durin::uint64 NextPackageId = 1;
		const std::string Name = std::string(Label) + std::to_string(NextPackageId++);
		Durin::FAssetPath Path;
		EXPECT_TRUE(Durin::FAssetPath::TryCreate("/EditorRevisionTests/" + Name, Path));
		Durin::DPackage* Package = Durin::NewObject<Durin::DPackage>(nullptr, Durin::FName(Name));
		Package->InitializeAssetPackage(Path);
		return Package;
	}

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

	class FTestComponentVisualizer final : public Durin::IComponentEditorVisualizer
	{
	public:
		explicit FTestComponentVisualizer(int* InDrawCount = nullptr)
			: DrawCount(InDrawCount)
		{
		}

		auto DrawVisualization(Durin::DActorComponent* Component, const Durin::FEditorVisualizationContext&, Durin::FEditorVisualizationCollector& Collector) const -> void override
		{
			if (DrawCount) ++*DrawCount;
			auto* SceneComponent = Durin::Cast<Durin::DSceneComponent>(Component);
			Durin::AActor* Actor = SceneComponent ? SceneComponent->GetOwner() : nullptr;
			if (!Actor) return;
			const Durin::FVector3 Center = SceneComponent->GetWorldLocation();
			Collector.AddLine({Center - Durin::FVectorConstants::Right, Center + Durin::FVectorConstants::Right, Durin::FVector4f(1.0f), 2.0f, 8.0f, 5, Actor, Component});
		}

	private:
		int* DrawCount = nullptr;
	};

	class FTestDetailsCustomization final : public Durin::IObjectDetailsCustomization
	{
	public:
		auto CustomizeDetails(Durin::FLevelEditorContext&, Durin::DObject*,
			Durin::FObjectPropertyViewBuilder&) -> void override {}
	};

	struct FCustomizationGuard
	{
		Durin::FLevelEditorCustomizationHandle Handle;
		~FCustomizationGuard() { if (Handle) Durin::FLevelEditorCustomizationRegistry::Get().Unregister(Handle); }
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

}
