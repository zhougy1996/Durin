#include "StaticMesh/StaticMeshCompilation.h"
#include "Widgets/MaterialPreview.h"
#include "Widgets/MaterialPreviewFraming.h"
#include "Graph/MaterialExpressionParameters.h"
#include "Graph/MaterialGraphValueTypes.h"

#include "Asset/AssetCompilingManager.h"
#include "DObject/Package.h"
#include "Materials/MaterialAttributeDefinitionMap.h"
#include "Materials/MaterialExpressionIO.h"

#include "Asset/AssetRetention.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Actor.h"
#include "Materials/MaterialCompilationConfiguration.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Materials/DefaultMaterialService.h"
#include "Math/Operations.h"
#include "MonaImGui.h"
#include "Preview/AssetPreviewHost.h"
#include "Preview/PreviewMeshPaths.h"
#include "SceneView.h"
#include "SceneViewProjection.h"
#include "StaticMesh/StaticMesh.h"
#include "DynamicRHI.h"

namespace Durin::MaterialEditor
{
	namespace
	{
		auto IsPostProcessMaterial(DMaterialInterface* Material) -> bool
		{
			if (!Material) return false;
			while (Material->GetParent()) Material = Material->GetParent();
			const auto* Root = Cast<DMaterial>(Material);
			return Root && Root->GetDomain() == EMaterialDomain::PostProcess;
		}
		auto IsWaterMaterial(const DMaterialInterface* Material) -> bool
		{
			return Material && Material->GetStaticProperties().ShadingModel == EMaterialShadingModel::SingleLayerWater;
		}
		constexpr float PreviewRotationSensitivity = 0.25f;
		constexpr double PreviewMinZoom = 0.4;
		constexpr double PreviewMaxZoom = 4.0;
		constexpr double PreviewZoomScale = 0.85;

		auto FeatureLevelLabel(ERHIFeatureLevel FeatureLevel) -> const char*
		{
			switch (FeatureLevel)
			{
			case ERHIFeatureLevel::ES3_1: return "ES3_1";
			case ERHIFeatureLevel::SM5: return "SM5";
			case ERHIFeatureLevel::SM6: return "SM6";
			}
			return "Unsupported";
		}

		// Selects the mesh used to visualize a material in the preview scene.
		enum class EMaterialPreviewShape : uint8
		{
			Sphere,
			Box
		};

		// Builds the orbiting scene view for the material preview viewport.
		class FMaterialPreviewViewportClient final : public FAssetPreviewViewportClient
		{
		public:
			auto Zoom(float MouseWheel) -> void
			{
				ZoomScale = std::clamp(
					ZoomScale * std::pow(PreviewZoomScale, static_cast<double>(MouseWheel)),
					PreviewMinZoom, PreviewMaxZoom);
			}

			auto Fit() -> void { ZoomScale = 1.0; }
			auto SetRadius(double InRadius) -> void { Radius = std::max(InRadius, 0.01); }

			auto CalcSceneView(uint32 Width, uint32 Height, FSceneView& OutView) const -> bool override
			{
				if (!IsPreviewEnabled() || Width == 0 || Height == 0) return false;

				constexpr float NearClip = 0.1f;
				const float AspectRatio = static_cast<float>(Width) / static_cast<float>(Height);
				const double Distance = CalculateMaterialPreviewDistance(Radius, AspectRatio) * ZoomScale;
				const float FarClip = static_cast<float>(std::max(100.0, Distance + Radius * 2.0));
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

				if (!SceneViewProjection::BuildPerspectiveProjection(MaterialPreviewFieldOfView,
					AspectRatio, NearClip, FarClip, ESceneDepthConvention::ReversedZ,
					OutView.ProjectionMatrix)) return false;
				OutView.NearClipDistance = NearClip;
				OutView.FarClipDistance = FarClip;
				OutView.DepthConvention = ESceneDepthConvention::ReversedZ;
				OutView.ViewProjectionMatrix = OutView.ProjectionMatrix * OutView.ViewMatrix;
				return true;
			}

		private:
			double ZoomScale = 1.0;
			double Radius = 1.0;
		};
	}

	// Owns the preview world, mesh actors, viewport, and camera state.
	class FMaterialPreview::FImpl
	{
	public:
		explicit FImpl(std::string_view PreviewId)
		{
			auto Client = std::make_unique<FMaterialPreviewViewportClient>();
			ViewportClient = Client.get();
			Host = std::make_unique<FAssetPreviewHost>(
				FAssetPreviewHostConfig{
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

			PreviewFeatureLevel = Host->GetFeatureLevel();
			FSoftObjectPath SpherePath;
			FSoftObjectPath BoxPath;
			if (const auto Validation = FSoftObjectPath::TryCreateWithDiagnostic(PreviewMeshPaths::SphereAssetPath, SpherePath); !Validation)
			{
				Error = ToString(Validation.error());
				return;
			}
			if (const auto Validation = FSoftObjectPath::TryCreateWithDiagnostic(PreviewMeshPaths::BoxAssetPath, BoxPath); !Validation)
			{
				Error = ToString(Validation.error());
				return;
			}
			if (!FAssetRetentionService::Acquire(SpherePath, SphereAsset, Error)
				|| !FAssetRetentionService::Acquire(BoxPath, BoxAsset, Error))
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
			if (WaterBed) WaterBed->SetStaticMesh(nullptr);
			Host.reset();
			if (RootInputPreview) FAssetCompilingManager::Get().MarkCompilationAsCanceled(*RootInputPreview);
		}

		auto SetVisible(bool bInVisible) -> void
		{
			if (Host) Host->SetVisible(bInVisible);
		}

		auto SetMaterial(DMaterialInterface* Material) -> void { UpdateScene(Material); }

		auto Draw(DMaterialInterface* Material, float PanelHeight,
			const std::optional<std::pair<EMaterialProperty, FMaterialInputDefault>>* RootInputDraft = nullptr) -> void
		{
			if (!ImGui::BeginChild("MaterialPreview", ImVec2(0.0f, PanelHeight), ImGuiChildFlags_None))
			{
				SetVisible(false);
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
			if (RootInputDraft)
				if (auto* Base = Cast<DMaterial>(Material)) Material = SelectRootInputPreview(*Base, *RootInputDraft);
			if (!IsWaterMaterial(Material))
			{
				const char* ShapeLabel = Shape == EMaterialPreviewShape::Sphere ? "Sphere" : "Box";
				ImGui::SetNextItemWidth(MonaImGui::ScaleUI(120.0f));
				if (ImGui::BeginCombo(IsPostProcessMaterial(Material) ? "Input Mesh" : "Preview Mesh", ShapeLabel))
				{
					for (const EMaterialPreviewShape Candidate : {EMaterialPreviewShape::Sphere, EMaterialPreviewShape::Box})
					{
						const char* Label = Candidate == EMaterialPreviewShape::Sphere ? "Sphere" : "Box";
						if (ImGui::Selectable(Label, Candidate == Shape))
						{
							Shape = Candidate;
							bProxyDirty = true;
							ViewportClient->Fit();
						}
					}
					ImGui::EndCombo();
				}
				ImGui::SameLine();
			}

			if (ImGui::Button("Fit")) ViewportClient->Fit();
			const ERHIFeatureLevel Capability = GDynamicRHI
				&& GDynamicRHI->RHIGetCapabilities()
				? GDynamicRHI->RHIGetCapabilities()->FeatureLevel
				: ERHIFeatureLevel::SM5;
			if (PreviewFeatureLevel > Capability) PreviewFeatureLevel = Capability;
			ImGui::SameLine();
			ImGui::SetNextItemWidth(MonaImGui::ScaleUI(90.0f));
			if (ImGui::BeginCombo("Quality", PreviewQuality == EMaterialQualityLevel::Low
				? "Low" : "High"))
			{
				for (const auto Candidate : SupportedMaterialQualityLevels)
					if (ImGui::Selectable(Candidate == EMaterialQualityLevel::Low ? "Low" : "High",
						Candidate == PreviewQuality))
					{
						PreviewQuality = Candidate;
						bProxyDirty = true;
						if (Material) Material->RequestPreviewProgramCompile(
							PreviewQuality, PreviewFeatureLevel);
					}
				ImGui::EndCombo();
			}
			ImGui::SameLine();
			ImGui::SetNextItemWidth(MonaImGui::ScaleUI(90.0f));
			if (ImGui::BeginCombo("Feature", FeatureLevelLabel(PreviewFeatureLevel)))
			{
				for (const auto Candidate : SupportedMaterialFeatureLevels)
				{
					if (Candidate > Capability) continue;
					if (ImGui::Selectable(FeatureLevelLabel(Candidate),
						Candidate == PreviewFeatureLevel))
					{
						if (Host->SetFeatureLevel(Candidate))
						{
							PreviewFeatureLevel = Candidate;
							PreviewMesh = Cast<DStaticMeshComponent>(Host->GetContentActor()->AddInstanceComponent(
								DStaticMeshComponent::StaticClass(), "PreviewMesh"));
							WaterBed = nullptr;
							CurrentMaterial = nullptr;
							bProxyDirty = true;
							if (Material) Material->CacheResourceShadersForRendering(PreviewFeatureLevel);
						}
						else Error = Host->GetDiagnostic();
					}
				}
				ImGui::EndCombo();
			}
			bool bEnableBloom = ViewportClient->GetViewSettings().PostProcess.bEnableBloom;
			if (ImGui::Checkbox("Bloom", &bEnableBloom))
			{
				auto Settings = ViewportClient->GetViewSettings();
				Settings.PostProcess.bEnableBloom = bEnableBloom;
				ViewportClient->SetViewSettings(Settings);
			}
			UpdateScene(Material);
			if (IsPostProcessMaterial(Material)) ImGui::TextUnformatted("PostProcess: studio scene before tone mapping");
			if (!SceneStatus.empty())
			{
				SetVisible(false);
				ImGui::TextWrapped("%s", SceneStatus.c_str());
				ImGui::EndChild();
				return;
			}
			const ImVec2 Available = ImGui::GetContentRegionAvail();
			const float Width = std::max(8.0f, Available.x);
			const float Height = std::max(8.0f, Available.y);
			FAssetPreviewViewportInput Input;
			if (Host->DrawViewport(Width, Height, &Input)) UpdateViewportInput(Input);
			ImGui::EndChild();
		}

		auto OnRootInputCommitted(uint64 PreviousRevision, uint64 CurrentRevision) -> void
		{
			if (!RootInputPreview || RootInputPreviewRevision != PreviousRevision) return;
			RootInputPreviewRevision = CurrentRevision;
			bRootInputPreviewCommitted = true;
		}

	private:
		auto BuildRootInputPreview(DMaterial& Working) -> void
		{
			if (RootInputPreview) FAssetCompilingManager::Get().MarkCompilationAsCanceled(*RootInputPreview);
			RootInputPreview.Reset();
			RootInputPreviewParameters.clear();
			RootInputPreviewRevision = Working.GetMaterialContentRevision();
			bRootInputPreviewCommitted = false;
			if (Working.UsesMaterialAttributes()) return;

			auto Root = Working.CaptureRootInputs();
			std::vector<DMaterialExpression*> Expressions;
			for (const auto& Expression : Working.GetExpressionCollection().Expressions)
				Expressions.push_back(Expression.Get());
			std::vector<TStrongObjectPtr<DMaterialExpression>> Added;
			const auto& Properties = Working.GetStaticProperties();
			for (const auto Property : FMaterialAttributeDefinitionMap::GetActiveProperties(
				Working.GetDomain(), Properties))
			{
				auto& Input = Root.Get(Property);
				if (Input.Connection.IsConnected() || !SupportsMaterialRootConstant(Property)) continue;
				const auto Type = GetMaterialSurfaceOutputType(Property);
				const auto Values = ReadMaterialNumericDefault(Input, Property);
				const FMaterialProgramLiteral Literal{Values[0], Values.size() > 1 ? Values[1] : 0.0f, Values.size() > 2 ? Values[2] : 0.0f};
				const auto Id = FGuid::NewGuid();
				auto Parameter = GraphEditInternals::MakeParameterExpression({
					.Id = Id, .Name = FName("Preview_" + Id.ToString()), .Type = *GetParameterType(Type),
					.Value = MakeParameterValue(Type, Literal)});
				if (!Parameter) return;
				auto* Output = static_cast<DMaterialExpression*>(Parameter.Expression.Get());
				Added.emplace_back(Output);
				Expressions.push_back(Output);
				if (Type == EMaterialProgramValueType::Float3)
				{
					auto Mask = GraphEditInternals::MakeParameterMask(*Parameter.Expression, Type);
					Output = Mask.Get();
					Added.emplace_back(Output);
					Expressions.push_back(Output);
				}
				Input.Connection = {Output};
				RootInputPreviewParameters.emplace(Property, Id);
			}
			if (RootInputPreviewParameters.empty()) return;

			TStrongObjectPtr<DMaterial> PreviewMaterial(NewObject<DMaterial>(
				DMaterial::StaticClass(), nullptr, NAME_None, EObjectFlags::Transient));
			PreviewMaterial->SetEditCompileMode(EMaterialEditCompileMode::Manual);
			FScopedPackageDirtySuppression SuppressPackageRevision;
			if (!PreviewMaterial->SetMaterialConfiguration({Working.GetDomain(), Properties})
				|| !PreviewMaterial->SetMaterialExpressions(Expressions, std::move(Root))) return;
			PreviewMaterial->CompileEdits();
			RootInputPreview = std::move(PreviewMaterial);
		}

		auto SelectRootInputPreview(DMaterial& Working,
				const std::optional<std::pair<EMaterialProperty, FMaterialInputDefault>>& Draft) -> DMaterial*
		{
			if (RootInputPreviewRevision != Working.GetMaterialContentRevision()) BuildRootInputPreview(Working);
			const bool bShowCommitted = bRootInputPreviewCommitted
				&& Working.GetEditCompileMode() != EMaterialEditCompileMode::Manual
				&& !Working.GetMaterialCompileStatus().IsCurrent();
			if ((!Draft && !bShowCommitted) || !RootInputPreview || !RootInputPreview->GetMaterialCompileStatus().IsCurrent())
				return &Working;

			auto SetValue = [this](const FGuid& Id, FMaterialParameterValue Value) {
				const auto* Existing = RootInputPreview->FindParameterDefinition(Id);
				if (Existing && Existing->Value != Value)
					(void)RootInputPreview->SetParameterValue(Id, std::move(Value));
			};
			FScopedPackageDirtySuppression SuppressPackageRevision;
			for (const auto& Parameter : Working.GetParameterDefinitions()) SetValue(Parameter.Id, Parameter.Value);
			for (const auto& [Property, Id] : RootInputPreviewParameters)
			{
				const auto Type = GetMaterialSurfaceOutputType(Property);
				const auto Values = ReadMaterialNumericDefault(*Working.GetNumericInputForProperty(Property), Property);
				const FMaterialProgramLiteral Literal = Draft && Draft->first == Property ? Draft->second.Literal
					: FMaterialProgramLiteral{Values[0], Values.size() > 1 ? Values[1] : 0.0f, Values.size() > 2 ? Values[2] : 0.0f};
				SetValue(Id, Type == EMaterialProgramValueType::Float3
					? FMaterialParameterValue::MakeVector4({Literal.X, Literal.Y, Literal.Z, 0.0})
					: MakeParameterValue(Type, Literal));
			}
			return RootInputPreview.Get();
		}

		auto UpdateViewportInput(const FAssetPreviewViewportInput& Input) -> void
		{
			if (!bWaterInput && Input.bLeftDragging && (Input.MouseDeltaX != 0.0f || Input.MouseDeltaY != 0.0f))
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
			const bool bPostProcess = IsPostProcessMaterial(Material);
			const bool bWater = IsWaterMaterial(Material);
			if (bProxyDirty || Material != CurrentMaterial || bPostProcess != bPostProcessInput || bWater != bWaterInput)
			{
				DStaticMesh* Mesh = bWater ? Cast<DStaticMesh>(BoxAsset.Get()) : GetSelectedMesh();
				if (Mesh == nullptr || Mesh->GetRenderData() == nullptr)
				{
					SceneStatus = Mesh && Mesh->IsCompiling()
						? "Building preview mesh..." : "The selected material preview mesh has no render data.";
					return;
				}
				SceneStatus.clear();
				PreviewMesh->SetStaticMesh(Mesh);
				PreviewMesh->SetWorldScale3D(FVector3(1.0));
				PreviewMesh->SetWorldLocation(FVector3(0.0));
				if (!bWater && WaterBed) WaterBed->SetStaticMesh(nullptr);
				if (const auto Bounds = Mesh->GetLOD0LocalBounds())
				{
					const auto Extent = Bounds->GetExtent();
					ViewportClient->SetRadius(Math::Length(Bounds->GetCenter()) + (!bWater && Shape == EMaterialPreviewShape::Sphere
						? std::max({Extent.x, Extent.y, Extent.z}) : Math::Length(Extent)));
					if (bWater)
					{
						if (!WaterBed) WaterBed = Cast<DStaticMeshComponent>(Host->GetContentActor()->AddInstanceComponent(
							DStaticMeshComponent::StaticClass(), "WaterPreviewBed"));
						if (!WaterBed) { SceneStatus = "The water preview bed could not be created."; return; }
						const double Top = Bounds->GetCenter().z + Extent.z;
						const double SurfaceScale = 0.005 / std::max(Extent.z, 0.001);
						const double BedScale = 0.1 / std::max(Extent.z, 0.001);
						PreviewMesh->SetWorldScale3D({1.0, 1.0, SurfaceScale});
						PreviewMesh->SetWorldLocation({0.0, 0.0, -Top * SurfaceScale});
						WaterBed->SetStaticMesh(Mesh);
						WaterBed->SetWorldScale3D({1.5, 1.5, BedScale});
						WaterBed->SetWorldLocation({0.0, 0.0, -0.65 - Top * BedScale});
						for (uint32 Slot = 0; Slot < WaterBed->GetNumMaterials(); ++Slot)
							WaterBed->SetMaterial(Slot, GetDefaultMaterial());
					}
				}
				ViewportClient->SetPostProcessMaterial(bPostProcess ? Material : nullptr,
					bPostProcess ? Material->GetRenderProxy(PreviewFeatureLevel, PreviewQuality) : FMaterialRenderProxyRef{});
				for (uint32 SlotIndex = 0; SlotIndex < PreviewMesh->GetNumMaterials(); ++SlotIndex)
					PreviewMesh->SetMaterial(SlotIndex, bPostProcess ? GetDefaultMaterial() : Material);
				PreviewMesh->SetWorldRotation(bWater ? FQuatConstants::Identity : PreviewRotation);
				CurrentMaterial = Material;
				bPostProcessInput = bPostProcess;
				bWaterInput = bWater;
				bProxyDirty = false;
			}
		}

		std::unique_ptr<FAssetPreviewHost> Host;
		FMaterialPreviewViewportClient* ViewportClient = nullptr;
		FRetainedAsset SphereAsset;
		FRetainedAsset BoxAsset;
		TObjectPtr<DStaticMeshComponent> PreviewMesh;
		TObjectPtr<DStaticMeshComponent> WaterBed;
		DMaterialInterface* CurrentMaterial = nullptr;
		TStrongObjectPtr<DMaterial> RootInputPreview;
		std::map<EMaterialProperty, FGuid> RootInputPreviewParameters;
		uint64 RootInputPreviewRevision = 0;
		bool bRootInputPreviewCommitted = false;
		FQuat PreviewRotation = FQuatConstants::Identity;
		EMaterialPreviewShape Shape = EMaterialPreviewShape::Sphere;
		EMaterialQualityLevel PreviewQuality = EMaterialQualityLevel::High;
		ERHIFeatureLevel PreviewFeatureLevel = ERHIFeatureLevel::SM5;
		bool bProxyDirty = true;
		bool bPostProcessInput = false;
		bool bWaterInput = false;
		// Mesh readiness is retried each frame; initialization errors are terminal.
		std::string SceneStatus;
		std::string Error;
	};

	FMaterialPreview::FMaterialPreview(std::string_view PreviewId)
		: Impl(std::make_unique<FImpl>(PreviewId))
	{
	}

	FMaterialPreview::~FMaterialPreview() = default;

	auto FMaterialPreview::SetVisible(bool bInVisible) -> void
	{
		Impl->SetVisible(bInVisible);
	}

	auto FMaterialPreview::SetMaterial(DMaterialInterface* Material) -> void
	{
		Impl->SetMaterial(Material);
	}

	auto FMaterialPreview::Draw(DMaterialInterface* Material, float PanelHeight) -> void
	{
		Impl->Draw(Material, PanelHeight);
	}
	auto FMaterialPreview::DrawRootInputs(DMaterialInterface* Material, float PanelHeight,
		const std::optional<std::pair<EMaterialProperty, FMaterialInputDefault>>& Draft) -> void
	{
		Impl->Draw(Material, PanelHeight, &Draft);
	}

	auto FMaterialPreview::OnRootInputCommitted(uint64 PreviousRevision, uint64 CurrentRevision) -> void
	{
		Impl->OnRootInputCommitted(PreviousRevision, CurrentRevision);
	}

}
