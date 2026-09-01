#pragma once

#include "Actors/SkyBoxActor.h"
#include "Asset/AssetCompilingManager.h"
#include "Asset/AssetOperations.h"
#include "Asset/Mutation.h"
#include "Asset/PackageSerialization.h"
#include "Asset/AssetCook.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkyBoxComponent.h"
#include "CoreGlobals.h"
#include "DObject/Class.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DynamicRHI.h"
#include "EngineTestSupport.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Rendering/SkyBoxSceneProxy.h"
#include "Materials/Material.h"
#include "Misc/Paths.h"
#include "Misc/MountPathTestSupport.h"
#include "Modules/ModuleManager.h"
#include "NativeTestSupport.h"
#include "RendererModule.h"
#include "Renderers/DisplayMapping.h"
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

#include <algorithm>
#include <cmath>

namespace
{
	auto PublishSkyBox(
		Durin::FScene& Scene,
		Durin::FSkyBoxSceneId SceneId,
		Durin::FSceneCandidateIdentity Identity,
		Durin::FSkyBoxSceneData Data) -> void
	{
		Scene.AddOrReplaceSkyBox(SceneId,
			std::make_unique<Durin::FSkyBoxSceneProxy>(
				std::move(Identity), std::move(Data)));
	}

	struct FObserveSkyBoxCommand
	{
		static constexpr auto GetName() -> const char* { return "ObserveSkyBox"; }
	};

	struct FSkyBoxObservation
	{
		bool bHasActive = false;
		Durin::FSkyBoxSceneSnapshot Active;
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
			Durin::FRendererModule SceneFactory;
			MainScene = SceneFactory.CreateScene();
			auto* Result = static_cast<Durin::FScene*>(MainScene.get());
			return Result;
		}

		auto ResetTestScene() -> void { MainScene.reset(); }
	};

	auto ObserveSkyBoxes(const Durin::FSceneInterface& SceneInterface)
		-> FSkyBoxObservation
	{
		const auto& Scene = static_cast<const Durin::FScene&>(SceneInterface);
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
		Durin::FModuleManager::Get().LoadModuleChecked("TextureBuild");
		Durin::FModuleManager::Get().LoadModuleChecked("AssetForgeBuiltins");
		const std::filesystem::path Root = Durin::Testing::GetTestWorkDirectory() / "SkyBoxAssets";
		static std::unordered_set<std::filesystem::path> InitializedRoots;
		if (InitializedRoots.insert(Root).second)
		{
			Durin::Testing::RemoveTestWorkDirectory(Root);
			Durin::Testing::RegisterMountPointForTests("/SkyBoxAssetTests/", Root.generic_string() + "/");
		}
		return Root;
	}

	auto ReconstructSampleDirection(const Durin::SkyBoxRendering::FSkyBoxUniform& Uniform, const Durin::FVector2& ClipPosition)
		-> Durin::FVector3
	{
		const Durin::FMatrix ClipToSkyDirection =
			glm::transpose(Durin::FMatrix(Uniform.ClipToSkyDirection));
		return glm::normalize(Durin::FVector3(ClipToSkyDirection
			* Durin::FVector4(ClipPosition, 1.0, 1.0)));
	}

	auto MakePrincipalAxisView(
		const Durin::FVector3& Direction,
		const Durin::FVector3& Location,
		uint32 Width,
		uint32 Height
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
		const Durin::FTextureCubeSourceData& SourceData,
		Durin::ETextureCubeFace Face,
		uint32 X,
		uint32 Y
	) -> std::array<uint8, 4>
	{
		const Durin::FTextureSourceData& Source =
			SourceData.Faces[static_cast<size_t>(Face)];
		const size_t PixelOffset = (static_cast<size_t>(Y) * Source.Width + X) * 4;
		return {
			std::to_integer<uint8>(Source.Pixels[PixelOffset]),
			std::to_integer<uint8>(Source.Pixels[PixelOffset + 1]),
			std::to_integer<uint8>(Source.Pixels[PixelOffset + 2]),
			std::to_integer<uint8>(Source.Pixels[PixelOffset + 3])
		};
	}

	auto MapSrgbReferenceThroughDisplay(
		const std::array<uint8, 4>& Source) -> std::array<uint8, 4>
	{
		auto Decode = [](uint8 Value) {
			const float Encoded = static_cast<float>(Value) / 255.0f;
			return Encoded <= 0.04045f
				? Encoded / 12.92f
				: std::pow((Encoded + 0.055f) / 1.055f, 2.4f);
		};
		auto Encode = [](float Linear) {
			const float Encoded = Linear <= 0.0031308f
				? 12.92f * Linear
				: 1.055f * std::pow(Linear, 1.0f / 2.4f) - 0.055f;
			return static_cast<uint8>(std::lround(
				std::clamp(Encoded, 0.0f, 1.0f) * 255.0f));
		};
		const Durin::FVector3f Mapped =
			Durin::DisplayMapping::MapSceneLinearToDisplayLinear(
				{Decode(Source[0]), Decode(Source[1]), Decode(Source[2])},
				0.0f);
		return {Encode(Mapped.x), Encode(Mapped.y), Encode(Mapped.z), Source[3]};
	}

	auto ExpectRgbNear(
		const Durin::FByteArray& Pixels,
		uint32 Width,
		uint32 X,
		uint32 Y,
		const std::array<uint8, 4>& Expected,
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
		const Durin::FByteArray& Actual,
		const Durin::FByteArray& Expected,
		uint32 Width,
		uint32 X,
		uint32 Y,
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
		const Durin::FByteArray& Actual,
		const std::array<Durin::FByteArray, Durin::TextureCubeFaceCount>& Candidates,
		uint32 Width
	) -> size_t
	{
		const size_t Offset = (static_cast<size_t>(Width / 2) * Width + Width / 2) * 4;
		size_t ClosestIndex = 0;
		uint32 ClosestDistance = std::numeric_limits<uint32>::max();
		for (size_t CandidateIndex = 0; CandidateIndex < Candidates.size(); ++CandidateIndex)
		{
			uint32 Distance = 0;
			for (size_t Channel = 0; Channel < 3; ++Channel)
			{
				const int Difference = static_cast<int>(Actual[Offset + Channel])
					- static_cast<int>(Candidates[CandidateIndex][Offset + Channel]);
				Distance += static_cast<uint32>(Difference * Difference);
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
