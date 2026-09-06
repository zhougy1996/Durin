#include "StaticMesh/StaticMeshCompilation.h"
#include "Widgets/MaterialPreview.h"

#include "Asset/AssetRetention.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Actor.h"
#include "Materials/MaterialInterface.h"
#include "Math/Operations.h"
#include "MonaImGui.h"
#include "Preview/AssetPreviewHost.h"
#include "SceneView.h"
#include "SceneViewProjection.h"
#include "StaticMesh/StaticMesh.h"

namespace Durin::Editor::Material
{
	namespace
	{
		constexpr float PreviewRotationSensitivity = 0.25f;
		constexpr double PreviewMinDistance = 1.5;
		constexpr double PreviewMaxDistance = 12.0;
		constexpr double PreviewZoomScale = 0.85;
		constexpr std::string_view PreviewSpherePath = "/Engine/Models/Sphere.Sphere";
		constexpr std::string_view PreviewBoxPath = "/Engine/Models/Box.Box";

		// Selects the mesh used to visualize a material in the preview scene.
		enum class EMaterialPreviewShape : uint8
		{
			Sphere,
			Box
		};

		// Builds the orbiting scene view for the material preview viewport.
		class FMaterialPreviewViewportClient final : public ::Durin::Editor::FAssetPreviewViewportClient
		{
		public:
			auto Zoom(float MouseWheel) -> void
			{
				Distance = std::clamp(
					Distance * std::pow(PreviewZoomScale, static_cast<double>(MouseWheel)),
					PreviewMinDistance, PreviewMaxDistance);
			}

			auto CalcSceneView(uint32 Width, uint32 Height, FSceneView& OutView) const -> bool override
			{
				if (!IsPreviewEnabled() || Width == 0 || Height == 0) return false;

				constexpr float FieldOfViewDegrees = 42.0f;
				constexpr float NearClip = 0.1f;
				constexpr float FarClip = 100.0f;
				const FVector3 Eye = Math::Normalize(FVector3(2.6, -2.6, 1.8)) * Distance;
				const FVector3 Forward = Math::Normalize(-Eye);
				const FVector3 Right = Math::Normalize(Math::Cross(FVectorConstants::Up, Forward));
				const FVector3 Up = Math::Normalize(Math::Cross(Forward, Right));

				OutView = {};
				OutView.ViewportWidth = Width;
				OutView.ViewportHeight = Height;
				OutView.ViewLocation = Eye;
				OutView.ViewMatrix[0][0] = Forward.x;
				OutView.ViewMatrix[1][0] = Forward.y;
				OutView.ViewMatrix[2][0] = Forward.z;
				OutView.ViewMatrix[3][0] = -Math::Dot(Forward, Eye);
				OutView.ViewMatrix[0][1] = Right.x;
				OutView.ViewMatrix[1][1] = Right.y;
				OutView.ViewMatrix[2][1] = Right.z;
				OutView.ViewMatrix[3][1] = -Math::Dot(Right, Eye);
				OutView.ViewMatrix[0][2] = Up.x;
				OutView.ViewMatrix[1][2] = Up.y;
				OutView.ViewMatrix[2][2] = Up.z;
				OutView.ViewMatrix[3][2] = -Math::Dot(Up, Eye);

				const float AspectRatio = static_cast<float>(Width) / static_cast<float>(Height);
				if (!SceneViewProjection::BuildPerspectiveProjection(FieldOfViewDegrees,
					AspectRatio, NearClip, FarClip, ESceneDepthConvention::ReversedZ,
					OutView.ProjectionMatrix)) return false;
				OutView.NearClipDistance = NearClip;
				OutView.FarClipDistance = FarClip;
				OutView.DepthConvention = ESceneDepthConvention::ReversedZ;
				OutView.ViewProjectionMatrix = OutView.ProjectionMatrix * OutView.ViewMatrix;
				return true;
			}

		private:
			double Distance = 4.1;
		};
	}

	// Owns the preview world, mesh actors, viewport, and camera state.
	class FMaterialPreview::FImpl
	{
	public:
		explicit FImpl(uint64 PreviewId)
		{
			auto Client = std::make_unique<FMaterialPreviewViewportClient>();
			ViewportClient = Client.get();
			Host = std::make_unique<::Durin::Editor::FAssetPreviewHost>(
				::Durin::Editor::FAssetPreviewHostConfig{
					.SceneName = FName(std::format("MaterialPreview_{}", PreviewId)),
					.ContentActorName = FName(std::format("MaterialPreviewActor_{}", PreviewId)),
					.LightActorName = FName(std::format("MaterialPreviewLightActor_{}", PreviewId)),
					.LightComponentName = FName(std::format("MaterialPreviewLight_{}", PreviewId))},
				std::move(Client));
			if (!Host->IsAvailable())
			{
				Error = Host->GetDiagnostic();
				return;
			}

			FObjectPath SpherePath;
			FObjectPath BoxPath;
			if (!FObjectPath::TryCreate(PreviewSpherePath, SpherePath, &Error)
				|| !FObjectPath::TryCreate(PreviewBoxPath, BoxPath, &Error)
				|| !::Durin::Editor::FAssetRetentionService::Acquire(SpherePath, SphereAsset, Error)
				|| !::Durin::Editor::FAssetRetentionService::Acquire(BoxPath, BoxAsset, Error))
			{
				return;
			}

			PreviewMesh = Host->GetContentActor()
				? Cast<DStaticMeshComponent>(Host->GetContentActor()->AddInstanceComponent(
					DStaticMeshComponent::StaticClass(), "PreviewMesh"))
				: nullptr;
			if (PreviewMesh == nullptr)
			{
				Error = "The material preview component could not be created.";
				return;
			}
		}

		~FImpl()
		{
			if (PreviewMesh) PreviewMesh->SetStaticMesh(nullptr);
			Host.reset();
		}

		auto SetVisible(bool bInVisible) -> void
		{
			if (Host) Host->SetVisible(bInVisible);
		}

		auto Draw(DMaterialInterface* Material, float PanelHeight) -> void
		{
			if (!ImGui::BeginChild("MaterialPreview", ImVec2(0.0f, PanelHeight), ImGuiChildFlags_Borders))
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
			ImGui::SeparatorText("Preview");
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
			::Durin::Editor::FAssetPreviewViewportInput Input;
			if (Host->DrawViewport(Width, Height, &Input)) UpdateViewportInput(Input);
			ImGui::EndChild();
		}

	private:
		auto UpdateViewportInput(const ::Durin::Editor::FAssetPreviewViewportInput& Input) -> void
		{
			if (Input.bLeftDragging && (Input.MouseDeltaX != 0.0f || Input.MouseDeltaY != 0.0f))
			{
				const FVector3 CameraForward = Math::Normalize(FVector3(-2.6, 2.6, -1.8));
				const FVector3 CameraRight = Math::Normalize(Math::Cross(FVectorConstants::Up, CameraForward));
				const FQuat Yaw = Math::MakeQuaternionFromAxisAngleDegrees(
					-static_cast<double>(Input.MouseDeltaX * PreviewRotationSensitivity),
					FVectorConstants::Up);
				const FQuat Pitch = Math::MakeQuaternionFromAxisAngleDegrees(
					-static_cast<double>(Input.MouseDeltaY * PreviewRotationSensitivity),
					CameraRight);
				PreviewRotation = Math::Normalize(Yaw * Pitch * PreviewRotation);
				if (PreviewMesh != nullptr) PreviewMesh->SetWorldRotation(PreviewRotation);
			}
			if (Input.MouseWheel != 0.0f) ViewportClient->Zoom(Input.MouseWheel);
		}

		auto GetSelectedMesh() const -> DStaticMesh*
		{
			return Cast<DStaticMesh>(Shape == EMaterialPreviewShape::Sphere ? SphereAsset.Get() : BoxAsset.Get());
		}

		auto UpdateScene(DMaterialInterface* Material) -> void
		{
			if (PreviewMesh == nullptr || Material == nullptr) return;
			if (bProxyDirty || Material != CurrentMaterial)
			{
				DStaticMesh* Mesh = GetSelectedMesh();
				if (Mesh == nullptr || Mesh->GetRenderData() == nullptr)
				{
					Error = Mesh && HasPendingStaticMeshCompilation(*Mesh)
						? "Building preview mesh..." : "The selected material preview mesh has no render data.";
					return;
				}
				Error.clear();
				PreviewMesh->SetStaticMesh(Mesh);
				for (uint32 SlotIndex = 0; SlotIndex < PreviewMesh->GetNumMaterials(); ++SlotIndex)
					PreviewMesh->SetMaterial(SlotIndex, Material);
				PreviewMesh->SetWorldRotation(PreviewRotation);
				CurrentMaterial = Material;
				bProxyDirty = false;
			}
		}

		std::unique_ptr<::Durin::Editor::FAssetPreviewHost> Host;
		FMaterialPreviewViewportClient* ViewportClient = nullptr;
		::Durin::Editor::FRetainedAsset SphereAsset;
		::Durin::Editor::FRetainedAsset BoxAsset;
		TObjectPtr<DStaticMeshComponent> PreviewMesh;
		DMaterialInterface* CurrentMaterial = nullptr;
		FQuat PreviewRotation = FQuatConstants::Identity;
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

	auto FMaterialPreview::Draw(DMaterialInterface* Material, float PanelHeight) -> void
	{
		Impl->Draw(Material, PanelHeight);
	}
}
