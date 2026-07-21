#include "Widgets/MaterialPreview.h"

#include "Client/ViewportClient.h"
#include "Components/DirectionalLightComponent.h"
#include "DObject/DObjectGlobals.h"
#include "Engine/Engine.h"
#include "Engine/PrimitiveSceneProxy.h"
#include "IRendererModule.h"
#include "IScene.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Paths.h"
#include "Mona/SceneViewport.h"
#include "MonaImGui.h"
#include "RenderingThread.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Widgets/MViewport.h"

namespace Durin
{
	namespace
	{
		constexpr FPrimitiveSceneId PreviewPrimitiveId = 1;
		constexpr double RotationTolerance = 1.0e-8;

		auto RotationFromForward(const FVector3& Direction) -> FQuat
		{
			const FVector3 To = glm::normalize(Direction);
			const double Dot = glm::dot(FVectorConstants::Forward, To);
			if (Dot > 1.0 - RotationTolerance) return glm::identity<FQuat>();
			if (Dot < -1.0 + RotationTolerance) return glm::angleAxis(glm::pi<double>(), FVectorConstants::Up);
			const FVector3 Cross = glm::cross(FVectorConstants::Forward, To);
			return glm::normalize(FQuat(1.0 + Dot, Cross.x, Cross.y, Cross.z));
		}

		enum class EMaterialPreviewShape : uint8
		{
			Sphere,
			Box
		};

		class FMaterialPreviewViewportClient final : public FViewportClient
		{
		public:
			auto SetEnabled(bool bInEnabled) -> void { bEnabled = bInEnabled; }

			auto CalcSceneView(uint32 Width, uint32 Height, FSceneView& OutView) const -> bool override
			{
				if (!bEnabled || Width == 0 || Height == 0) return false;

				constexpr float FieldOfViewDegrees = 42.0f;
				constexpr float NearClip = 0.1f;
				constexpr float FarClip = 100.0f;
				const FVector3 Eye(2.6, -2.6, 1.8);
				const FVector3 Forward = glm::normalize(-Eye);
				const FVector3 Right = glm::normalize(glm::cross(FVectorConstants::Up, Forward));
				const FVector3 Up = glm::normalize(glm::cross(Forward, Right));

				OutView = {};
				OutView.ViewportWidth = Width;
				OutView.ViewportHeight = Height;
				OutView.ViewLocation = Eye;
				OutView.ViewMatrix[0][0] = Forward.x;
				OutView.ViewMatrix[1][0] = Forward.y;
				OutView.ViewMatrix[2][0] = Forward.z;
				OutView.ViewMatrix[3][0] = -glm::dot(Forward, Eye);
				OutView.ViewMatrix[0][1] = Right.x;
				OutView.ViewMatrix[1][1] = Right.y;
				OutView.ViewMatrix[2][1] = Right.z;
				OutView.ViewMatrix[3][1] = -glm::dot(Right, Eye);
				OutView.ViewMatrix[0][2] = Up.x;
				OutView.ViewMatrix[1][2] = Up.y;
				OutView.ViewMatrix[2][2] = Up.z;
				OutView.ViewMatrix[3][2] = -glm::dot(Up, Eye);

				const float AspectRatio = static_cast<float>(Width) / static_cast<float>(Height);
				const float YScale = 1.0f / std::tan(glm::radians(FieldOfViewDegrees) * 0.5f);
				const float XScale = YScale / std::max(AspectRatio, 0.001f);
				const float DepthScale = FarClip / (FarClip - NearClip);
				const float DepthBias = -NearClip * FarClip / (FarClip - NearClip);
				OutView.ProjectionMatrix = FMatrix(0.0f);
				OutView.ProjectionMatrix[1][0] = XScale;
				OutView.ProjectionMatrix[2][1] = -YScale;
				OutView.ProjectionMatrix[0][2] = DepthScale;
				OutView.ProjectionMatrix[3][2] = DepthBias;
				OutView.ProjectionMatrix[0][3] = 1.0f;
				OutView.ViewProjectionMatrix = OutView.ProjectionMatrix * OutView.ViewMatrix;
				return true;
			}

		private:
			bool bEnabled = false;
		};
	}

	class FMaterialPreview::FImpl
	{
	public:
		explicit FImpl(uint64 PreviewId)
		{
			if (GEngine == nullptr || GEngine->GetRendererModule() == nullptr)
			{
				Error = "The renderer is not available.";
				return;
			}

			PreviewScene = GEngine->GetRendererModule()->CreateScene();
			const std::string ContentRoot = FPaths::EngineContentDir() + "Editor/MaterialPreview/";
			Sphere = DStaticMesh::CreateTransientFromFile(
				ContentRoot + "Sphere.obj", GEngine, std::format("MaterialPreviewSphere_{}", PreviewId), Error);
			if (Sphere == nullptr) return;
			Box = DStaticMesh::CreateTransientFromFile(
				ContentRoot + "Box.obj", GEngine, std::format("MaterialPreviewBox_{}", PreviewId), Error);
			if (Box == nullptr) return;

			PreviewLight = NewObject<DDirectionalLightComponent>(GEngine, std::format("MaterialPreviewLight_{}", PreviewId));
			// Aim the key light from just above the camera so color and specular edits remain readable on every shape.
			PreviewLight->SetWorldRotation(RotationFromForward(FVector3(-2.6, 2.6, -2.4)));
			PreviewScene->AddDirectionalLight(PreviewLight);

			ViewportClient = std::make_unique<FMaterialPreviewViewportClient>();
			ViewportWidget = std::make_shared<MViewport>();
			SceneViewport = std::make_shared<FSceneViewport>(ViewportClient.get(), ViewportWidget, PreviewScene.get());
			ViewportWidget->SetViewportInterface(SceneViewport);
			GEngine->RegisterAuxiliarySceneViewport(SceneViewport);
		}

		~FImpl()
		{
			if (GEngine != nullptr) GEngine->UnregisterAuxiliarySceneViewport(SceneViewport.get());
			SceneViewport.reset();
			ViewportWidget.reset();
			ViewportClient.reset();
			if (PreviewScene != nullptr)
			{
				PreviewScene->Release();
				// Scene commands capture the scene pointer, so its storage must outlive the release command.
				if (GRenderingThread) FlushRenderingCommands();
				PreviewScene.reset();
			}
		}

		auto SetVisible(bool bInVisible) -> void
		{
			if (ViewportClient != nullptr) ViewportClient->SetEnabled(bInVisible);
		}

		auto Draw(DMaterialInterface* Material) -> void
		{
			if (!ImGui::BeginChild("MaterialPreview", ImVec2(0.0f, MonaImGui::ScaleUI(300.0f)), ImGuiChildFlags_Borders))
			{
				ImGui::EndChild();
				return;
			}

			if (!Error.empty())
			{
				ImGui::TextWrapped("Material preview unavailable: %s", Error.c_str());
				ImGui::EndChild();
				return;
			}

			SetVisible(true);
			const char* ShapeLabel = Shape == EMaterialPreviewShape::Sphere ? "Sphere" : "Box";
			ImGui::SetNextItemWidth(MonaImGui::ScaleUI(120.0f));
			if (ImGui::BeginCombo("Preview Mesh", ShapeLabel))
			{
				for (const EMaterialPreviewShape Candidate : {EMaterialPreviewShape::Sphere, EMaterialPreviewShape::Box})
				{
					const char* Label = Candidate == EMaterialPreviewShape::Sphere ? "Sphere" : "Box";
					if (ImGui::Selectable(Label, Candidate == Shape))
					{
						Shape = Candidate;
						bProxyDirty = true;
					}
				}
				ImGui::EndCombo();
			}

			UpdateScene(Material);
			const ImVec2 Available = ImGui::GetContentRegionAvail();
			const float Width = std::max(8.0f, Available.x);
			const float Height = std::max(8.0f, Available.y);
			ViewportWidget->SetDesiredSize({Width, Height});
			ViewportWidget->Draw();
			ImGui::EndChild();
		}

	private:
		auto GetSelectedMesh() const -> DStaticMesh*
		{
			return Shape == EMaterialPreviewShape::Sphere ? Sphere.Get() : Box.Get();
		}

		auto MakeMaterialUpdates(DMaterialInterface* Material) -> std::vector<FMaterialRenderUpdate>
		{
			DStaticMesh* Mesh = GetSelectedMesh();
			const FStaticMeshRenderData* RenderData = Mesh != nullptr ? Mesh->GetRenderData() : nullptr;
			const uint32 SlotCount = RenderData != nullptr ? static_cast<uint32>(RenderData->MaterialSlots.size()) : 0;
			std::vector<FMaterialRenderUpdate> Updates;
			Updates.reserve(SlotCount);
			for (uint32 SlotIndex = 0; SlotIndex < SlotCount; ++SlotIndex)
			{
				Updates.push_back({
					.SlotIndex = SlotIndex,
					.RenderData = Material != nullptr ? Material->GetRenderData() : FMaterialRenderData{},
					.MaterialVersion = Material != nullptr ? Material->GetRenderStateVersion() : 0,
					.ComponentRevision = MaterialRevision,
					.DirtyFlags = EMaterialRenderDirtyFlags::ParameterValues | EMaterialRenderDirtyFlags::ParentChain,
				});
			}
			return Updates;
		}

		auto UpdateScene(DMaterialInterface* Material) -> void
		{
			if (PreviewScene == nullptr || Material == nullptr) return;
			const uint64 Version = Material->GetRenderStateVersion();
			if (bProxyDirty || Material != CurrentMaterial)
			{
				++MaterialRevision;
				DStaticMesh* Mesh = GetSelectedMesh();
				PreviewScene->AddOrReplacePrimitive(
					PreviewPrimitiveId,
					std::make_unique<FStaticMeshSceneProxy>(Mesh->GetRenderData(), MakeMaterialUpdates(Material)),
					FMatrix(1.0));
				CurrentMaterial = Material;
				MaterialVersion = Version;
				bProxyDirty = false;
				return;
			}
			if (Version == MaterialVersion) return;

			++MaterialRevision;
			for (FMaterialRenderUpdate& Update : MakeMaterialUpdates(Material))
			{
				Update.ComponentRevision = MaterialRevision;
				PreviewScene->UpdatePrimitiveMaterial(PreviewPrimitiveId, Update);
			}
			MaterialVersion = Version;
		}

		std::unique_ptr<IScene> PreviewScene;
		std::unique_ptr<FMaterialPreviewViewportClient> ViewportClient;
		std::shared_ptr<MViewport> ViewportWidget;
		std::shared_ptr<FSceneViewport> SceneViewport;
		TObjectPtr<DStaticMesh> Sphere;
		TObjectPtr<DStaticMesh> Box;
		TObjectPtr<DDirectionalLightComponent> PreviewLight;
		DMaterialInterface* CurrentMaterial = nullptr;
		uint64 MaterialVersion = 0;
		uint64 MaterialRevision = 1;
		EMaterialPreviewShape Shape = EMaterialPreviewShape::Sphere;
		bool bProxyDirty = true;
		std::string Error;
	};

	FMaterialPreview::FMaterialPreview(uint64 PreviewId)
		: Impl(std::make_unique<FImpl>(PreviewId))
	{
	}

	FMaterialPreview::~FMaterialPreview() = default;

	auto FMaterialPreview::SetVisible(bool bInVisible) -> void
	{
		Impl->SetVisible(bInVisible);
	}

	auto FMaterialPreview::Draw(DMaterialInterface* Material) -> void
	{
		Impl->Draw(Material);
	}
}
