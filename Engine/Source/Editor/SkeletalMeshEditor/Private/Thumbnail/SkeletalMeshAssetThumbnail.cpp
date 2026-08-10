#include "Thumbnail/SkeletalMeshAssetThumbnail.h"

#include "AssetSystem.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Actor.h"
#include "Engine/World.h"
#include "Math/Operations.h"
#include "SkeletalMesh/SkeletalMesh.h"

namespace Durin
{
	namespace
	{
		auto MakeFingerprint(const Asset::FAssetData& Data)
			-> FAssetThumbnailPackageFingerprint
		{
			return {.VirtualPath = Data.PackagePath,
				.AssetClassName = Data.AssetClassName,
				.PackageFormatVersion = Data.FormatVersion,
				.FileSize = static_cast<uint64>(Data.FileSize),
				.LastWriteTimeTicks = Data.LastWriteTimeTicks};
		}

		auto Qualify(const FAssetPath& Path, std::string_view Detail) -> std::string
		{
			return std::format("SkeletalMesh '{}' thumbnail generation failed: {}",
				Path.ToString(), Detail.empty() ? "unknown preview error" : Detail);
		}

		class FSession final : public IRenderedAssetThumbnailGenerationSession
		{
		public:
			explicit FSession(FSkeletalMeshAssetThumbnailGenerationInput InInput)
				: Input(std::move(InInput)) {}
			~FSession() override { ResetPreview(); }

			auto Load() -> FRenderedAssetThumbnailSessionUpdate override
			{
				DObject* Loaded = nullptr;
				const Asset::FAssetResult Result = Asset::LoadAsset(Input.AssetPath, Loaded);
				Mesh = Result ? Cast<DSkeletalMesh>(Loaded) : nullptr;
				if (!Result || !Mesh || Mesh->GetClass() != DSkeletalMesh::StaticClass())
					return {.State = ERenderedAssetThumbnailSessionState::Failed,
						.Diagnostic = Result.Message.empty()
							? Qualify(Input.AssetPath, "the asset is not an exact DSkeletalMesh")
							: Result.Message};
				const FSkeletalMeshSummary& Summary = Mesh->GetSummary();
				if (!Mesh->GetSkeleton() || !Summary.LocalBounds.IsValid())
					return {.State = ERenderedAssetThumbnailSessionState::Failed,
						.Diagnostic = Qualify(Input.AssetPath, "Skeleton or finite bounds are unavailable")};
				FSkeletalMeshRenderResourceStatus Status = Mesh->GetRenderResourceStatus();
				if (Status.Readiness == ESkeletalMeshRenderResourceReadiness::Unavailable)
				{
					Mesh->InitResources(); Status = Mesh->GetRenderResourceStatus();
				}
				AssetRevision = Status.Revision;
				return {.State = ERenderedAssetThumbnailSessionState::WaitingForResources,
					.AssetRevision = AssetRevision};
			}

			auto PollResources() -> FRenderedAssetThumbnailSessionUpdate override
			{
				if (!Mesh) return {.State = ERenderedAssetThumbnailSessionState::Failed,
					.Diagnostic = Qualify(Input.AssetPath, "the asset is unavailable")};
				const FSkeletalMeshRenderResourceStatus Status = Mesh->GetRenderResourceStatus();
				if (Status.Readiness == ESkeletalMeshRenderResourceReadiness::Ready)
				{
					AssetRevision = Status.Revision;
					return {.State = ERenderedAssetThumbnailSessionState::ReadyToRender,
						.AssetRevision = AssetRevision, .ResourceRevision = Status.Revision};
				}
				if (Status.Readiness == ESkeletalMeshRenderResourceReadiness::Queued)
					return {.State = ERenderedAssetThumbnailSessionState::WaitingForResources,
						.AssetRevision = AssetRevision, .ResourceRevision = Status.Revision};
				return {.State = ERenderedAssetThumbnailSessionState::Failed,
					.AssetRevision = AssetRevision, .ResourceRevision = Status.Revision,
					.Diagnostic = Qualify(Input.AssetPath, "render resources failed or became unavailable")};
			}

			auto PreparePreview(IRenderedAssetThumbnailPreviewScene& Scene,
				std::string& OutError) -> bool override
			{
				ResetPreview();
				if (!Mesh) { OutError = Qualify(Input.AssetPath, "the asset is unavailable"); return false; }
				const FBox Bounds = Mesh->GetSummary().LocalBounds.ToBox();
				const FVector3 Center = Bounds.GetCenter();
				const FVector3 Extent = Bounds.GetExtent();
				const double Radius = std::max({Extent.x, Extent.y, Extent.z});
				FVector3 Direction(Input.Visual.CameraDirectionX,
					Input.Visual.CameraDirectionY, Input.Visual.CameraDirectionZ);
				if (!Math::TryNormalize(Direction, Direction) || !std::isfinite(Radius) || Radius <= 0.0)
				{
					OutError = Qualify(Input.AssetPath, "the reference-pose framing input is invalid"); return false;
				}
				const FVector3 Forward = -Direction;
				FVector3 Right; FVector3 Up;
				if (!Math::TryNormalize(Math::Cross(FVectorConstants::Up, Forward), Right)
					|| !Math::TryNormalize(Math::Cross(Forward, Right), Up))
				{
					OutError = Qualify(Input.AssetPath, "the camera basis is invalid"); return false;
				}
				const double Distance = Radius * 1.5
					/ std::tan(Math::DegreesToRadians(Input.Visual.VerticalFieldOfViewDegrees * 0.5));
				World = Scene.GetWorld();
				Actor = World ? World->SpawnActor<AActor>("SkeletalMeshThumbnailPreviewActor") : nullptr;
				Component = Actor ? Cast<DSkeletalMeshComponent>(Actor->AddInstanceComponent(
					DSkeletalMeshComponent::StaticClass(), "SkeletalMeshPreview")) : nullptr;
				if (!Component || !Component->SetSkeletalMesh(Mesh, OutError))
				{
					OutError = Qualify(Input.AssetPath, OutError); ResetPreview(); return false;
				}
				Component->Pause(); Component->ClearMaterialOverrides();
				const FVector3 CameraPosition = Center + Direction * Distance;
				const FRenderedAssetThumbnailPreviewView View{
					.CameraPosition = {CameraPosition.x, CameraPosition.y, CameraPosition.z},
					.CameraForward = {Forward.x, Forward.y, Forward.z},
					.CameraRight = {Right.x, Right.y, Right.z},
					.CameraUp = {Up.x, Up.y, Up.z},
					.VerticalFieldOfViewDegrees = Input.Visual.VerticalFieldOfViewDegrees,
					.NearClipDistance = std::max(1.0e-4, Distance - Radius * 2.0),
					.FarClipDistance = Distance + Radius * 2.0,
					.bForceLOD0 = true,
					.ClearAlpha = 0.0f};
				if (!Scene.SetView(View, OutError))
				{
					OutError = Qualify(Input.AssetPath, OutError); ResetPreview(); return false;
				}
				return true;
			}

			auto ValidateRevisions(uint64 ExpectedAssetRevision,
				uint64 ExpectedResourceRevision, std::string& OutError) const -> bool override
			{
				const FSkeletalMeshRenderResourceStatus Status = Mesh
					? Mesh->GetRenderResourceStatus() : FSkeletalMeshRenderResourceStatus{};
				if (!Status.IsReady() || Status.Revision != ExpectedAssetRevision
					|| Status.Revision != ExpectedResourceRevision)
				{
					OutError = std::format("SkeletalMesh '{}' changed during thumbnail generation.",
						Input.AssetPath.ToString()); return false;
				}
				OutError.clear(); return true;
			}

			auto ResetPreview() -> void override
			{
				if (World && Actor) World->DestroyActor(Actor);
				Component = nullptr; Actor = nullptr; World = nullptr;
			}

		private:
			FSkeletalMeshAssetThumbnailGenerationInput Input;
			DSkeletalMesh* Mesh = nullptr; uint64 AssetRevision = 0;
			DWorld* World = nullptr; AActor* Actor = nullptr;
			DSkeletalMeshComponent* Component = nullptr;
		};
	}

	auto FSkeletalMeshAssetThumbnailProvider::GetRegistration() const
		-> FAssetThumbnailProviderRegistration
	{
		return {.AssetClassName = DSkeletalMesh::StaticClass()->GetQualifiedName().ToString(),
			.ProviderName = std::string(FSkeletalMeshAssetThumbnailContract::ProviderName),
			.GeneratorSchemaVersion = FSkeletalMeshAssetThumbnailContract::GeneratorSchemaVersion};
	}

	auto FSkeletalMeshAssetThumbnailProvider::CaptureGenerationRequest(
		const FAssetThumbnailRequest& Request, uint64 ProviderGeneration,
		FAssetThumbnailGenerationRequest& OutRequest, std::string& OutError) -> bool
	{
		OutRequest = {}; OutError.clear();
		if (Request.Asset.AssetClassName != GetRegistration().AssetClassName)
		{
			OutError = "The skeletal thumbnail provider received the wrong asset class."; return false;
		}
		const Asset::FAssetRegistry& Registry = Asset::GetAssetRegistry();
		const Asset::FAssetData* Root = Registry.FindAssetExact(Request.Asset.VirtualPath);
		if (!Root || MakeFingerprint(*Root) != Request.Asset)
		{
			OutError = "Skeletal thumbnail registry data is missing or changed."; return false;
		}
		std::vector<FAssetThumbnailDependencyNode> Nodes;
		Nodes.reserve(Registry.GetAssets().size());
		for (const auto& [Path, Data] : Registry.GetAssets())
			Nodes.push_back({.Package = MakeFingerprint(Data), .Dependencies = Data.Dependencies});
		std::vector<FAssetThumbnailPackageFingerprint> Dependencies;
		if (!BuildAssetThumbnailDependencyClosure(Request.Asset.VirtualPath,
			Nodes, Dependencies, OutError)) return false;
		FRenderedAssetThumbnailVisualContract Visual;
		Visual.CameraDirectionX = 2.4f; Visual.CameraDirectionY = -3.2f;
		Visual.CameraDirectionZ = 2.4f; Visual.VerticalFieldOfViewDegrees = 42.0f;
		Visual.bOutputOpaque = false;
		OutRequest.KeyInput = {.Output = Visual.Output,
			.PreviewFixtureIdentity = std::string(FSkeletalMeshAssetThumbnailContract::PreviewFixtureIdentity),
			.PreviewFixtureVersion = FSkeletalMeshAssetThumbnailContract::PreviewFixtureVersion,
			.ShaderContractVersion = FSkeletalMeshAssetThumbnailContract::ShaderContractVersion,
			.Dependencies = std::move(Dependencies)};
		OutRequest.Input = std::make_shared<FSkeletalMeshAssetThumbnailGenerationInput>(
			Request.Asset.VirtualPath, std::move(Visual));
		OutRequest.ProviderGeneration = ProviderGeneration;
		OutRequest.RequestSerial = Request.RequestSerial;
		return true;
	}

	auto FSkeletalMeshAssetThumbnailProvider::CreateGenerationSession(
		const FAssetThumbnailGenerationRequest&, const IAssetThumbnailGenerationInput& Input,
		std::string& OutError) -> std::unique_ptr<IRenderedAssetThumbnailGenerationSession>
	{
		const auto* Typed = dynamic_cast<const FSkeletalMeshAssetThumbnailGenerationInput*>(&Input);
		if (!Typed) { OutError = "The skeletal thumbnail generation input is invalid."; return nullptr; }
		OutError.clear(); return std::make_unique<FSession>(*Typed);
	}
}
