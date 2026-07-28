#pragma once

#include "Actors/SkyBoxActor.h"
#include "AssetSystem.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkyBoxComponent.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DynamicRHI.h"
#include "EngineTestSupport.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "Misc/Paths.h"
#include "NativeTestSupport.h"
#include "RendererModule.h"
#include "RHIGlobals.h"
#include "RHICommandList.h"
#include "Scene.h"
#include "RenderingThread.h"
#include "SkyBoxDetails.h"
#include "SkyBoxRendering.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/TextureCube.h"
#include "Texture/TextureCubeRenderResource.h"

#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace
{
	struct FObserveSkyBoxCommand
	{
		static constexpr auto GetName() -> const char* { return "ObserveSkyBox"; }
	};

	struct FSkyBoxObservation
	{
		bool bHasActive = false;
		Durin::FSkyBoxSceneData Active;
		size_t Count = 0;
	};

	class FSkyBoxTestEngine final : public Durin::DEngine
	{
	public:
		FSkyBoxTestEngine()
			: DEngine(Durin::FObjectInitializer::Get())
		{
		}

		auto CreateTestScene() -> Durin::FScene*
		{
			auto Scene = std::make_unique<Durin::FScene>();
			Durin::FScene* Result = Scene.get();
			MainScene = std::move(Scene);
			return Result;
		}

		auto ResetTestScene() -> void { MainScene.reset(); }
	};

	auto ObserveSkyBoxes(const Durin::FScene& Scene) -> FSkyBoxObservation
	{
		auto Result = std::make_shared<FSkyBoxObservation>();
		Durin::EnqueueRenderCommand<FObserveSkyBoxCommand>([&Scene, Result](Durin::FRHICommandListImmediate&) {
			Result->bHasActive = Scene.GetActiveSkyBox_RenderThread(Result->Active);
			Result->Count = Scene.GetSkyBoxCount_RenderThread();
		});
		Durin::FlushRenderingCommands();
		return *Result;
	}

	auto GetSkyBoxConventionFaces() -> std::array<std::string, Durin::TextureCubeFaceCount>
	{
		constexpr std::array<std::string_view, Durin::TextureCubeFaceCount> FaceNames = {
			"PositiveX", "NegativeX", "PositiveY", "NegativeY", "PositiveZ", "NegativeZ"};
		std::array<std::string, Durin::TextureCubeFaceCount> Result;
		for (size_t FaceIndex = 0; FaceIndex < Result.size(); ++FaceIndex)
		{
			Result[FaceIndex] = (std::filesystem::path(DURIN_TEST_DATA_DIR) / "SkyBoxConvention" /
				std::format("{}.png", FaceNames[FaceIndex])).generic_string();
		}
		return Result;
	}

	auto GetSkyBoxPanoramaFixture(std::string_view FileName) -> std::filesystem::path
	{
		return std::filesystem::path(DURIN_TEST_DATA_DIR) / "EquirectangularPanorama" / FileName;
	}

	auto InitializeSkyBoxAssetMount() -> std::filesystem::path
	{
		InitializeDObjectSystem();
		const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "SkyBoxAssets";
		static std::unordered_set<std::filesystem::path> InitializedRoots;
		if (InitializedRoots.insert(Root).second)
		{
			std::filesystem::remove_all(Root);
			Durin::PathUtilities::RegisterMountPoint("/SkyBoxAssetTests/", Root.generic_string() + "/");
		}
		return Root;
	}

	auto ReconstructSampleDirection(const Durin::SkyBoxRendering::FSkyBoxUniform& Uniform, const Durin::FVector2& ClipPosition)
		-> Durin::FVector3
	{
		const Durin::FMatrix ClipToWorld = glm::transpose(Durin::FMatrix(Uniform.ClipToWorld));
		const Durin::FMatrix WorldToSky = glm::transpose(Durin::FMatrix(Uniform.WorldToSky));
		const Durin::FVector4 WorldPositionH = ClipToWorld * Durin::FVector4(ClipPosition, 1.0, 1.0);
		const Durin::FVector3 WorldPosition = Durin::FVector3(WorldPositionH) / WorldPositionH.w;
		const Durin::FVector3 ViewPosition(Uniform.ViewPosition);
		return glm::normalize(Durin::FVector3(WorldToSky * Durin::FVector4(
			glm::normalize(WorldPosition - ViewPosition), 0.0)));
	}

	auto MakePrincipalAxisView(
		const Durin::FVector3& Direction,
		const Durin::FVector3& Location,
		Durin::uint32 Width,
		Durin::uint32 Height
	) -> Durin::FSceneView
	{
		const Durin::FVector3 Forward = glm::normalize(Direction);
		const Durin::FVector3 UpHint = std::abs(glm::dot(Forward, Durin::FVectorConstants::Up)) > 0.99
			? Durin::FVectorConstants::Right : Durin::FVectorConstants::Up;
		const Durin::FVector3 Right = glm::normalize(glm::cross(UpHint, Forward));
		const Durin::FVector3 Up = glm::cross(Forward, Right);
		Durin::FMatrix ClipToWorld(1.0);
		ClipToWorld[0] = Durin::FVector4(Right, 0.0);
		ClipToWorld[1] = Durin::FVector4(Up, 0.0);
		ClipToWorld[2] = Durin::FVector4(Forward, 0.0);
		ClipToWorld[3] = Durin::FVector4(Location, 1.0);
		Durin::FSceneView View;
		View.ViewLocation = Location;
		View.ViewProjectionMatrix = glm::inverse(ClipToWorld);
		View.ViewportWidth = Width;
		View.ViewportHeight = Height;
		return View;
	}

	auto GetSourceColor(
		const Durin::DTextureCube& Cube,
		Durin::ETextureCubeFace Face,
		Durin::uint32 X,
		Durin::uint32 Y
	) -> std::array<Durin::uint8, 4>
	{
		const Durin::FTextureSourceData& Source = Cube.GetSourceData()->Faces[static_cast<size_t>(Face)];
		const size_t PixelOffset = (static_cast<size_t>(Y) * Source.Width + X) * 4;
		return {
			Source.Pixels[PixelOffset],
			Source.Pixels[PixelOffset + 1],
			Source.Pixels[PixelOffset + 2],
			Source.Pixels[PixelOffset + 3]
		};
	}

	auto ExpectRgbNear(
		const std::vector<Durin::uint8>& Pixels,
		Durin::uint32 Width,
		Durin::uint32 X,
		Durin::uint32 Y,
		const std::array<Durin::uint8, 4>& Expected,
		int Tolerance = 20
	) -> void
	{
		const size_t Offset = (static_cast<size_t>(Y) * Width + X) * 4;
		ASSERT_LE(Offset + 4, Pixels.size());
		for (size_t Channel = 0; Channel < 3; ++Channel)
		{
			EXPECT_NEAR(static_cast<int>(Pixels[Offset + Channel]), static_cast<int>(Expected[Channel]), Tolerance);
		}
	}

	auto ExpectRgbMatch(
		const std::vector<Durin::uint8>& Actual,
		const std::vector<Durin::uint8>& Expected,
		Durin::uint32 Width,
		Durin::uint32 X,
		Durin::uint32 Y,
		int Tolerance = 2
	) -> void
	{
		const size_t Offset = (static_cast<size_t>(Y) * Width + X) * 4;
		ASSERT_LE(Offset + 4, Actual.size());
		ASSERT_LE(Offset + 4, Expected.size());
		for (size_t Channel = 0; Channel < 3; ++Channel)
		{
			EXPECT_NEAR(static_cast<int>(Actual[Offset + Channel]), static_cast<int>(Expected[Offset + Channel]), Tolerance);
		}
	}

	auto FindClosestCenterRgb(
		const std::vector<Durin::uint8>& Actual,
		const std::array<std::vector<Durin::uint8>, Durin::TextureCubeFaceCount>& Candidates,
		Durin::uint32 Width
	) -> size_t
	{
		const size_t Offset = (static_cast<size_t>(Width / 2) * Width + Width / 2) * 4;
		size_t ClosestIndex = 0;
		Durin::uint32 ClosestDistance = std::numeric_limits<Durin::uint32>::max();
		for (size_t CandidateIndex = 0; CandidateIndex < Candidates.size(); ++CandidateIndex)
		{
			Durin::uint32 Distance = 0;
			for (size_t Channel = 0; Channel < 3; ++Channel)
			{
				const int Difference = static_cast<int>(Actual[Offset + Channel])
					- static_cast<int>(Candidates[CandidateIndex][Offset + Channel]);
				Distance += static_cast<Durin::uint32>(Difference * Difference);
			}
			if (Distance < ClosestDistance)
			{
				ClosestDistance = Distance;
				ClosestIndex = CandidateIndex;
			}
		}
		return ClosestIndex;
	}
}
