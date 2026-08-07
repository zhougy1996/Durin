#include "StandardAssetImportProviders.h"

#include "ImportedScene.h"
#include "AssetImportCore.h"
#include "AssetSystem.h"
#include "DObject/ObjectLifecycle.h"
#include "Materials/MaterialInstance.h"
#include "SceneImport.h"
#include "SceneImportInternal.h"
#include "AsyncImport.h"
#include "StaticMeshImportAdapter.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/Texture2D.h"
#include "Texture/TextureCube.h"

namespace Durin
{
	namespace
	{
		using namespace AssetImport;

		template<typename T>
		auto AppendValue(std::vector<uint8>& Bytes, const T& Value) -> void
		{
			static_assert(std::is_trivially_copyable_v<T>);
			const auto* Begin = reinterpret_cast<const uint8*>(&Value);
			Bytes.insert(Bytes.end(), Begin, Begin + sizeof(T));
		}

		auto MakePayload(std::string SchemaId, uint32 Version, std::vector<uint8> Bytes)
			-> FImportPayload
		{
			FImportPayload Payload{
				.SchemaId = std::move(SchemaId),
				.SchemaVersion = Version,
				.Bytes = std::move(Bytes)};
			std::string Error;
			requiref(Payload.Finalize(Error), "{}", Error);
			return Payload;
		}

		auto MakeStaticMeshSettings(const FStaticMeshImportSettings& Settings) -> FImportPayload
		{
			std::vector<uint8> Bytes;
			AppendValue(Bytes, Settings.ForwardAxis);
			AppendValue(Bytes, Settings.RightAxis);
			AppendValue(Bytes, Settings.UpAxis);
			return MakePayload("Durin.StaticMesh.ImportSettings", 1, std::move(Bytes));
		}

		auto ImportAxisVector(EStaticMeshImportAxis Axis) -> FVector3f
		{
			switch (Axis)
			{
			case EStaticMeshImportAxis::PositiveX: return {1.0f, 0.0f, 0.0f};
			case EStaticMeshImportAxis::NegativeX: return {-1.0f, 0.0f, 0.0f};
			case EStaticMeshImportAxis::PositiveY: return {0.0f, 1.0f, 0.0f};
			case EStaticMeshImportAxis::NegativeY: return {0.0f, -1.0f, 0.0f};
			case EStaticMeshImportAxis::PositiveZ: return {0.0f, 0.0f, 1.0f};
			case EStaticMeshImportAxis::NegativeZ: return {0.0f, 0.0f, -1.0f};
			}
			return {};
		}

		auto MakeMeshImportOptions(
			const FStaticMeshImportSettings& Settings,
			const FSourcePath& RootSource) -> Asset::FMeshImportOptions
		{
			const FVector3f Forward = ImportAxisVector(Settings.ForwardAxis);
			const FVector3f Right = ImportAxisVector(Settings.RightAxis);
			const FVector3f Up = ImportAxisVector(Settings.UpAxis);
			Asset::FMeshImportOptions Options;
			for (uint32 Component = 0; Component < 3; ++Component)
			{
				Options.SourceToEngine[Component][0] = Forward[Component];
				Options.SourceToEngine[Component][1] = Right[Component];
				Options.SourceToEngine[Component][2] = Up[Component];
			}
			Options.RootSource = RootSource;
			return Options;
		}

		auto DecodeStaticMeshSource(
			std::string_view FilePath,
			const FStaticMeshImportSettings& Settings,
			FStaticMeshImportedData& OutData,
			std::string& OutError) -> bool
		{
			Asset::FImportedSceneData Scene;
			if (Asset::ImportFromFile(
				FilePath, Scene, MakeMeshImportOptions(Settings, {})))
			{
				OutData = MakeStaticMeshImportedData(Scene);
				OutError.clear();
				return true;
			}
			OutError = std::format("Failed to decode StaticMesh source {}.", FilePath);
			return false;
		}

		bool GStaticMeshSourceDecoderRegistered =
			RegisterStaticMeshSourceDecoder(&DecodeStaticMeshSource);

		auto MakeTexture2DSettings(const DTexture2D& Texture) -> FImportPayload
		{
			std::vector<uint8> Bytes;
			AppendValue(Bytes, Texture.GetUsage());
			AppendValue(Bytes, Texture.GetCompressionQuality());
			AppendValue(Bytes, Texture.GetAlphaMipMode());
			AppendValue(Bytes, Texture.GetAlphaCoverageThreshold());
			AppendValue(Bytes, Texture.GetMaxResolution());
			const bool bSRGB = Texture.IsSRGB();
			AppendValue(Bytes, bSRGB);
			return MakePayload("Durin.Texture2D.ImportSettings", 1, std::move(Bytes));
		}

		auto MakeTextureCubeSettings(const DTextureCube& Texture) -> FImportPayload
		{
			std::vector<uint8> Bytes;
			AppendValue(Bytes, Texture.GetSourceLayout());
			AppendValue(Bytes, Texture.GetPanoramaFaceDimension());
			AppendValue(Bytes, Texture.GetPanoramaExposureEV());
			const bool bSRGB = Texture.IsSRGB();
			AppendValue(Bytes, bSRGB);
			return MakePayload("Durin.TextureCube.ImportSettings", 1, std::move(Bytes));
		}

		auto MakeSourceHash(const FTextureSourceFile& Source) -> FXxHash128
		{
			FXxHash128 Hash;
			Hash.HashLow = Source.SourceContentHashLow;
			Hash.HashHigh = Source.SourceContentHashHigh;
			return Hash;
		}

		auto MakeCandidatePath(const FAssetPath& TargetPath, FAssetPath& OutPath) -> bool
		{
			for (uint32 Suffix = 1; Suffix != 0; ++Suffix)
			{
				if (!FAssetPath::TryCreate(std::format("{}_ImportCandidate_{}",
					TargetPath.ToString(), Suffix), OutPath)) return false;
				if (!Asset::FindLoadedPackage(OutPath)
					&& !Asset::GetAssetRegistry().FindAssetExact(OutPath)) return true;
			}
			return false;
		}

		class FEngineSingleAssetCandidate final : public ISingleAssetCandidate
		{
		public:
			explicit FEngineSingleAssetCandidate(DObject* InAsset)
				: AssetObject(InAsset), Package(InAsset ? InAsset->GetPackage() : nullptr) {}

			auto GetAsset() const -> DObject* override { return AssetObject; }
			auto GetPackage() const -> DPackage* override { return Package; }
			auto IsNewAsset() const -> bool override { return false; }
			auto GetAuthoredFingerprint() const -> std::string override
			{
				if (const auto* Mesh = Cast<DStaticMesh>(AssetObject))
					return Mesh->GetSourceImportData().SourceContentHash;
				if (const auto* Texture = Cast<DTexture2D>(AssetObject))
					return Texture->GetDerivedDataKey();
				if (const auto* Cube = Cast<DTextureCube>(AssetObject))
					return Cube->GetDerivedDataKey();
				return {};
			}
			auto Validate(std::vector<FImportDiagnostic>& OutDiagnostics) const -> bool override
			{
				bool bValid = false;
				if (const auto* Mesh = Cast<DStaticMesh>(AssetObject)) bValid = Mesh->GetRenderData() != nullptr;
				else if (const auto* Texture = Cast<DTexture2D>(AssetObject))
					bValid = Texture->GetPlatformData() != nullptr
						&& Texture->GetBuildStatus() == ETextureBuildStatus::Ready;
				else if (const auto* Cube = Cast<DTextureCube>(AssetObject))
					bValid = Cube->GetPlatformData() != nullptr
						&& Cube->GetBuildStatus() == ETextureBuildStatus::Ready;
				if (!bValid)
					OutDiagnostics.push_back({
						.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::ValidationFailure,
						.Phase = "candidate-validation",
						.Message = "Engine asset candidate has no validated runtime data."});
				return bValid;
			}
			auto Abandon() noexcept -> void override
			{
				if (!Package) return;
				(void)Asset::DiscardUnpublishedPackage(Package);
				Package = nullptr;
				AssetObject = nullptr;
			}

		private:
			DObject* AssetObject = nullptr;
			DPackage* Package = nullptr;
		};

		template<typename T>
		class TNoFailExchange final : public IPreparedImportedStateExchange
		{
		public:
			TNoFailExchange(T& InTarget, T& InCandidate)
				: Target(&InTarget), Candidate(&InCandidate) {}
			auto Commit() noexcept -> void override
			{
				if (!bCommitted) { Target->ExchangeImportedState(*Candidate); bCommitted = true; }
			}
			auto Reverse() noexcept -> void override
			{
				if (bCommitted) { Target->ExchangeImportedState(*Candidate); bCommitted = false; }
			}
			auto Finalize() noexcept -> void override { Target = nullptr; Candidate = nullptr; }
		private:
			T* Target = nullptr;
			T* Candidate = nullptr;
			bool bCommitted = false;
		};

		class FStaticMeshExchange final : public IPreparedImportedStateExchange
		{
		public:
			explicit FStaticMeshExchange(std::unique_ptr<FStaticMeshImportedStateExchange> InExchange)
				: Exchange(std::move(InExchange)) {}
			auto Commit() noexcept -> void override { Exchange->Commit(); }
			auto Reverse() noexcept -> void override { Exchange->Reverse(); }
			auto Finalize() noexcept -> void override { Exchange->Finalize(); }
		private:
			std::unique_ptr<FStaticMeshImportedStateExchange> Exchange;
		};

		class FIdentityProvider final : public IImportProvider
		{
		public:
			FIdentityProvider(std::string InId, uint32 InVersion, std::vector<std::string> InExtensions)
				: Id(std::move(InId)), Version(InVersion), Extensions(std::move(InExtensions)) {}
			auto GetProviderId() const -> std::string_view override { return Id; }
			auto GetContractVersion() const -> uint32 override { return Version; }
			auto CanImport(const FImportSourceRecognition& Source) const -> bool override
			{
				std::string Extension = Source.Extension;
				std::ranges::transform(Extension, Extension.begin(), [](unsigned char Character) {
					return static_cast<char>(std::tolower(Character));
				});
				return std::ranges::find(Extensions, Extension) != Extensions.end();
			}
			auto CaptureSettings(FImportPayload& OutSettings,
				std::vector<FImportDiagnostic>&) const -> bool override
			{
				OutSettings = MakePayload(std::format("Durin.{}.DefaultSettings", Id), 1, {});
				return true;
			}
			auto DiscoverDependencies(std::span<const FSourceSnapshotEntry>,
				FDependencyRequestSink&, std::vector<FImportDiagnostic>&) const -> bool override
			{
				return true;
			}
			auto Plan(const FSourceSnapshot&, const FImportPayload&, FImportPlanBuilder&,
				std::vector<FImportDiagnostic>&) const -> bool override { return true; }
		private:
			std::string Id;
			uint32 Version = 0;
			std::vector<std::string> Extensions;
		};

		auto MakeCapabilities(
			std::string ClassName,
			std::string ProviderId,
			bool bCanReimport,
			std::string ReplacementDescription) -> FSingleAssetCapabilitySet
		{
			FSingleAssetCapabilitySet Result{
				.AssetClassName = std::move(ClassName),
				.ProviderId = std::move(ProviderId)};
			Result.Capabilities = {
				{.Capability = ESingleAssetImportCapability::Import,
					.bAvailable = false, .Label = "Import as New Asset",
					.ReplacedStateDescription = "Creates a new authored asset."},
				{.Capability = ESingleAssetImportCapability::ReimportCurrentSource,
					.bAvailable = bCanReimport, .Label = "Reimport from Current Source",
					.ReplacedStateDescription = ReplacementDescription},
				{.Capability = ESingleAssetImportCapability::ReimportNewSource,
					.bAvailable = bCanReimport, .Label = "Reimport from Mounted Source",
					.ReplacedStateDescription = ReplacementDescription},
				{.Capability = ESingleAssetImportCapability::RepairSource,
					.bAvailable = true, .Label = "Repair Source Reference",
					.ReplacedStateDescription = "Replaces only persisted source identities, then rebuilds imported state."}};
			return Result;
		}

		class FStaticMeshHandler final : public ISingleAssetImportHandler
		{
		public:
			auto GetAssetClassName() const -> std::string_view override { return "Durin::DStaticMesh"; }
			auto GetProviderId() const -> std::string_view override { return "Assimp"; }
			auto InspectProvenance(const DObject& AssetObject, FSingleAssetProvenance& Out,
				std::vector<FImportDiagnostic>& Diagnostics) const -> bool override
			{
				const auto* Mesh = Cast<DStaticMesh>(&AssetObject);
				if (!Mesh || !Mesh->GetSourceImportData().HasSource()) return false;
				const FStaticMeshSourceImportData& Source = Mesh->GetSourceImportData();
				Out = {
					.ProviderId = Source.ImporterId,
					.ProviderContractVersion = Source.ImporterVersion,
					.Settings = MakeStaticMeshSettings(Source.ImportSettings),
					.Sources = {{
						.StableIdentity = "geometry",
						.Role = "Geometry",
						.SourcePath = Source.SourcePath,
						.ContentHash = FXxHash128::FromString(Source.SourceContentHash)}},
					.AuthoredOutputFingerprint = Source.SourceContentHash};
				if (!Out.IsComplete() || Source.SourceContentHash.size() != 32)
				{
					Diagnostics.push_back({
						.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::InvalidSource,
						.Phase = "provenance-inspection",
						.Message = "StaticMesh source provenance is incomplete."});
					return false;
				}
				return true;
			}
			auto QueryCapabilities(const DObject& AssetObject,
				const FSingleAssetProvenance&) const -> FSingleAssetCapabilitySet override
			{
				const auto* Mesh = Cast<DStaticMesh>(&AssetObject);
				bool bGeometryOnly = Mesh != nullptr;
				if (bGeometryOnly && Mesh->GetPackage())
				{
					FAssetPath MeshPath;
					std::string IndexError;
					bGeometryOnly = FAssetPath::TryCreate(
						Mesh->GetPackage()->GetPackagePath(), MeshPath)
						&& GetImportRecordIndex().EnsureCurrent(IndexError)
						&& GetImportRecordIndex().FindManagers(MeshPath).empty();
				}
				auto Result = MakeCapabilities(std::string(GetAssetClassName()), "Assimp", bGeometryOnly,
					"Replaces StaticMesh geometry, material-slot import metadata, source provenance, DDC identity, and render resources; object identity and component overrides are retained.");
				if (!bGeometryOnly)
				{
					for (auto& Capability : Result.Capabilities)
						if (Capability.Capability == ESingleAssetImportCapability::ReimportCurrentSource
							|| Capability.Capability == ESingleAssetImportCapability::ReimportNewSource)
							Capability.Diagnostics.push_back({
								.Severity = EImportDiagnosticSeverity::Error,
								.Category = EImportDiagnosticCategory::CapabilityUnavailable,
								.Phase = "capability-query",
								.Message = "This StaticMesh is the root of a legacy multi-output import; use its Scene reimport action until record migration."});
				}
				return Result;
			}
			auto BuildCandidate(const FSingleAssetImportPlan& Plan,
				std::vector<FImportDiagnostic>& Diagnostics) const
				-> std::unique_ptr<ISingleAssetCandidate> override
			{
				auto* Target = Cast<DStaticMesh>(Plan.GetAsset());
				const FSourceSnapshotEntry* Root = Plan.GetSnapshot().FindSource("root");
				FAssetPath CandidatePath;
				DStaticMesh* Candidate = nullptr;
				std::string Error;
				if (!Target || !Root || !MakeCandidatePath(Plan.GetAssetPath(), CandidatePath)
					|| !Asset::CreateAsset(CandidatePath, Candidate)) return nullptr;
				auto Result = std::make_unique<FEngineSingleAssetCandidate>(Candidate);
				Asset::FImportedSceneData Scene;
				if (!Asset::ImportGeometryFromMemory(
					Root->GetBytes(), std::filesystem::path(Root->SourcePath.Path).extension().generic_string(),
					Scene, MakeMeshImportOptions(Target->GetImportSettings(), Root->SourcePath)))
				{
					Diagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::CandidateFailure,
						.Phase = "candidate-build", .Message = "Captured geometry could not be decoded."});
					Result->Abandon();
					return nullptr;
				}
				Candidate->SeedMaterialReconciliationFrom(*Target);
				FStaticMeshSourceImportData Provenance{
					.SourcePath = Root->SourcePath,
					.SourceContentHash = Root->ContentHash.ToString(),
					.ImporterId = Plan.GetProvenance().ProviderId,
					.ImporterVersion = Plan.GetProvenance().ProviderContractVersion,
					.ImportSettings = Target->GetImportSettings()};
				if (!Candidate->InitializeFromImportedData(
					MakeStaticMeshImportedData(Scene), Provenance,
					Root->SourcePath.Path, Error))
				{
					Diagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::CandidateFailure,
						.Phase = "candidate-build", .Message = Error});
					Result->Abandon();
					return nullptr;
				}
				return Result;
			}
			auto PrepareImportedStateExchange(DObject& TargetObject, ISingleAssetCandidate& CandidateObject,
				std::vector<FImportDiagnostic>& Diagnostics) const
				-> std::unique_ptr<IPreparedImportedStateExchange> override
			{
				auto* Target = Cast<DStaticMesh>(&TargetObject);
				auto* Candidate = Cast<DStaticMesh>(CandidateObject.GetAsset());
				std::string Error;
				auto Exchange = Target && Candidate
					? Target->PrepareImportedStateExchange(*Candidate, Error) : nullptr;
				if (!Exchange)
					Diagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::CandidateFailure,
						.Phase = "exchange-prepare", .Message = Error});
				return Exchange ? std::make_unique<FStaticMeshExchange>(std::move(Exchange)) : nullptr;
			}
			auto RepairSource(DObject& AssetObject, std::span<const FSourcePath> Sources,
				std::vector<FImportDiagnostic>& Diagnostics) const -> bool override
			{
				auto* Mesh = Cast<DStaticMesh>(&AssetObject);
				std::string Error;
				const bool bResult = Mesh && Sources.size() == 1
					&& Mesh->ChangeSourceReference(Sources[0].Path, Error);
				if (!bResult) Diagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
					.Category = EImportDiagnosticCategory::InvalidSource,
					.Phase = "source-repair", .Message = Error});
				return bResult;
			}
		};

		class FTexture2DHandler final : public ISingleAssetImportHandler
		{
		public:
			auto GetAssetClassName() const -> std::string_view override { return "Durin::DTexture2D"; }
			auto GetProviderId() const -> std::string_view override { return "DurinImage"; }
			auto InspectProvenance(const DObject& AssetObject, FSingleAssetProvenance& Out,
				std::vector<FImportDiagnostic>&) const -> bool override
			{
				const auto* Texture = Cast<DTexture2D>(&AssetObject);
				if (!Texture || !Texture->GetSourceImportData().HasSource()) return false;
				const auto& Source = Texture->GetSourceImportData();
				Out = {.ProviderId = Source.DecoderId,
					.ProviderContractVersion = Source.DecoderVersion,
					.Settings = MakeTexture2DSettings(*Texture),
					.Sources = {{.StableIdentity = "image", .Role = "Image",
						.SourcePath = Source.Source.SourcePath,
						.ContentHash = MakeSourceHash(Source.Source)}},
					.AuthoredOutputFingerprint = Texture->GetDerivedDataKey()};
				return Out.IsComplete();
			}
			auto QueryCapabilities(const DObject&, const FSingleAssetProvenance&) const
				-> FSingleAssetCapabilitySet override
			{
				return MakeCapabilities(std::string(GetAssetClassName()), "DurinImage", true,
					"Replaces Texture2D decoded source, source provenance, import settings, mip/platform data, DDC identity, and render resource; object identity is retained.");
			}
			auto BuildCandidate(const FSingleAssetImportPlan& Plan,
				std::vector<FImportDiagnostic>& Diagnostics) const
				-> std::unique_ptr<ISingleAssetCandidate> override
			{
				auto* Target = Cast<DTexture2D>(Plan.GetAsset());
				const FSourceSnapshotEntry* Root = Plan.GetSnapshot().FindSource("root");
				FAssetPath CandidatePath;
				DTexture2D* Candidate = nullptr;
				if (!Target || !Root || !MakeCandidatePath(Plan.GetAssetPath(), CandidatePath)
					|| !Asset::CreateAsset(CandidatePath, Candidate)) return nullptr;
				auto Result = std::make_unique<FEngineSingleAssetCandidate>(Candidate);
				FTexture2DImportSettings Settings{
					.Usage = Target->GetUsage(),
					.CompressionQuality = Target->GetCompressionQuality(),
					.AlphaMipMode = Target->GetAlphaMipMode(),
					.AlphaCoverageThreshold = Target->GetAlphaCoverageThreshold(),
					.MaxResolution = Target->GetMaxResolution(),
					.bSRGB = Target->IsSRGB()};
				std::string Error;
				if (!Candidate->BuildFromEncodedBytes(Root->GetBytes(), Root->SourcePath, Settings, Error))
				{
					Diagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
						.Category = EImportDiagnosticCategory::CandidateFailure,
						.Phase = "candidate-build", .Message = Error});
					Result->Abandon();
					return nullptr;
				}
				return Result;
			}
			auto PrepareImportedStateExchange(DObject& TargetObject, ISingleAssetCandidate& CandidateObject,
				std::vector<FImportDiagnostic>&) const
				-> std::unique_ptr<IPreparedImportedStateExchange> override
			{
				auto* Target = Cast<DTexture2D>(&TargetObject);
				auto* Candidate = Cast<DTexture2D>(CandidateObject.GetAsset());
				return Target && Candidate ? std::make_unique<TNoFailExchange<DTexture2D>>(*Target, *Candidate) : nullptr;
			}
			auto RepairSource(DObject& AssetObject, std::span<const FSourcePath> Sources,
				std::vector<FImportDiagnostic>& Diagnostics) const -> bool override
			{
				auto* Texture = Cast<DTexture2D>(&AssetObject);
				std::string Error;
				const bool bResult = Texture && Sources.size() == 1
					&& Texture->ChangeSourceReference(Sources[0].Path, Error);
				if (!bResult) Diagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
					.Category = EImportDiagnosticCategory::InvalidSource,
					.Phase = "source-repair", .Message = Error});
				return bResult;
			}
		};

		class FTextureCubeHandler final : public ISingleAssetImportHandler
		{
		public:
			auto GetAssetClassName() const -> std::string_view override { return "Durin::DTextureCube"; }
			auto GetProviderId() const -> std::string_view override { return "DurinImage"; }
			auto InspectProvenance(const DObject& AssetObject, FSingleAssetProvenance& Out,
				std::vector<FImportDiagnostic>&) const -> bool override
			{
				const auto* Cube = Cast<DTextureCube>(&AssetObject);
				if (!Cube || !Cube->GetSourceImportData().HasSource()) return false;
				const auto& Source = Cube->GetSourceImportData();
				Out.ProviderId = Source.DecoderId;
				Out.ProviderContractVersion = Source.DecoderVersion;
				Out.Settings = MakeTextureCubeSettings(*Cube);
				Out.AuthoredOutputFingerprint = Cube->GetDerivedDataKey();
				if (Source.SourceLayout == ETextureCubeSourceLayout::EquirectangularPanorama)
					Out.Sources.push_back({.StableIdentity = "panorama", .Role = "Panorama",
						.SourcePath = Source.Panorama.SourcePath,
						.ContentHash = MakeSourceHash(Source.Panorama)});
				else for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
				{
					const auto Face = static_cast<ETextureCubeFace>(Index);
					const FTextureSourceFile& FaceSource = Source.GetFace(Face);
					Out.Sources.push_back({.StableIdentity = std::format("face-{}", Index),
						.Role = "CubeFace", .SourcePath = FaceSource.SourcePath,
						.ContentHash = MakeSourceHash(FaceSource)});
				}
				return Out.IsComplete();
			}
			auto QueryCapabilities(const DObject&, const FSingleAssetProvenance&) const
				-> FSingleAssetCapabilitySet override
			{
				return MakeCapabilities(std::string(GetAssetClassName()), "DurinImage", true,
					"Replaces all TextureCube source provenance, panorama projection or six-face settings, mip/platform data, DDC identity, and render resource; object identity is retained.");
			}
			auto BuildCandidate(const FSingleAssetImportPlan& Plan,
				std::vector<FImportDiagnostic>& Diagnostics) const
				-> std::unique_ptr<ISingleAssetCandidate> override
			{
				auto* Target = Cast<DTextureCube>(Plan.GetAsset());
				FAssetPath CandidatePath;
				DTextureCube* Candidate = nullptr;
				if (!Target || !MakeCandidatePath(Plan.GetAssetPath(), CandidatePath)
					|| !Asset::CreateAsset(CandidatePath, Candidate)) return nullptr;
				auto Result = std::make_unique<FEngineSingleAssetCandidate>(Candidate);
				std::string Error;
				if (Target->GetSourceLayout() == ETextureCubeSourceLayout::EquirectangularPanorama)
				{
					const FSourceSnapshotEntry* Root = Plan.GetSnapshot().FindSource("root");
					if (!Root || !Candidate->BuildPanoramaFromEncodedBytes(
						Root->GetBytes(), std::filesystem::path(Root->SourcePath.Path).extension().generic_string(),
						Root->SourcePath, {Target->GetPanoramaFaceDimension(), Target->GetPanoramaExposureEV()}, Error))
					{
						Diagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
							.Category = EImportDiagnosticCategory::CandidateFailure,
							.Phase = "candidate-build", .Message = Error});
						Result->Abandon();
						return nullptr;
					}
				}
				else
				{
					std::array<std::span<const uint8>, TextureCubeFaceCount> Bytes;
					std::array<FSourcePath, TextureCubeFaceCount> Paths;
					for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
					{
						const FSourceSnapshotEntry* Entry = Plan.GetSnapshot().FindSource(
							Index == 0 ? "root" : std::format("face-{}", Index));
						if (!Entry) { Result->Abandon(); return nullptr; }
						Bytes[Index] = Entry->GetBytes();
						Paths[Index] = Entry->SourcePath;
					}
					if (!Candidate->BuildFacesFromEncodedBytes(Bytes, Paths, {Target->IsSRGB()}, Error))
					{
						Diagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
							.Category = EImportDiagnosticCategory::CandidateFailure,
							.Phase = "candidate-build", .Message = Error});
						Result->Abandon();
						return nullptr;
					}
				}
				return Result;
			}
			auto PrepareImportedStateExchange(DObject& TargetObject, ISingleAssetCandidate& CandidateObject,
				std::vector<FImportDiagnostic>&) const
				-> std::unique_ptr<IPreparedImportedStateExchange> override
			{
				auto* Target = Cast<DTextureCube>(&TargetObject);
				auto* Candidate = Cast<DTextureCube>(CandidateObject.GetAsset());
				return Target && Candidate ? std::make_unique<TNoFailExchange<DTextureCube>>(*Target, *Candidate) : nullptr;
			}
			auto RepairSource(DObject& AssetObject, std::span<const FSourcePath> Sources,
				std::vector<FImportDiagnostic>& Diagnostics) const -> bool override
			{
				auto* Cube = Cast<DTextureCube>(&AssetObject);
				std::string Error;
				bool bResult = false;
				if (Cube && Cube->GetSourceLayout() == ETextureCubeSourceLayout::EquirectangularPanorama
					&& Sources.size() == 1)
					bResult = Cube->ChangePanoramaSourceReference(Sources[0].Path,
						{Cube->GetPanoramaFaceDimension(), Cube->GetPanoramaExposureEV()}, Error);
				else if (Cube && Sources.size() == TextureCubeFaceCount)
				{
					std::array<std::string, TextureCubeFaceCount> Paths;
					for (uint32 Index = 0; Index < TextureCubeFaceCount; ++Index)
						Paths[Index] = Sources[Index].Path;
					bResult = Cube->ChangeSourceReferences(Paths, {Cube->IsSRGB()}, Error);
				}
				if (!bResult) Diagnostics.push_back({.Severity = EImportDiagnosticSeverity::Error,
					.Category = EImportDiagnosticCategory::InvalidSource,
					.Phase = "source-repair", .Message = Error});
				return bResult;
			}
		};

		class FSceneRecordHandler final : public IImportRecordHandler
		{
		public:
			auto GetProviderId() const -> std::string_view override
			{
				return SceneImportProviderId;
			}

			auto QueryCapabilities(
				const DImportRecord&,
				const FImportRecordInspection& Inspection) const
				-> FImportRecordCapabilitySet override
			{
				const bool bCanPublish = !Inspection.bConflicted;
				auto Make = [&](EImportRecordAction Action, std::string Label,
					bool bAvailable) -> FImportRecordCapability
				{
					FImportRecordCapability Capability{
						.Action = Action,
						.bAvailable = bCanPublish && bAvailable,
						.Label = std::move(Label)};
					if (!Capability.bAvailable)
					{
						Capability.Diagnostics.push_back({
							.Severity = EImportDiagnosticSeverity::Error,
							.Category = Inspection.bConflicted
								? EImportDiagnosticCategory::Collision
								: EImportDiagnosticCategory::CapabilityUnavailable,
							.Phase = "capability-query",
							.SourceIdentity = "root",
							.OutputIdentity = "record",
							.Message = Inspection.bConflicted
								? "Repair the duplicated record identity or manager conflict first."
								: "The selected record does not require this action."});
						FinalizeImportDiagnostics(Capability.Diagnostics, "capability-query");
					}
					return Capability;
				};
				return {
					.ProviderId = std::string(SceneImportProviderId),
					.Capabilities = {
						Make(EImportRecordAction::Reimport, "Reimport Managed Outputs", true),
						Make(EImportRecordAction::RecreateMissingOutputs,
							"Recreate Missing Outputs", Inspection.bHasMissingManagedOutput),
						Make(EImportRecordAction::RepairManagedOutputs,
							"Repair Changed Outputs", Inspection.bHasFingerprintMismatch)}};
			}

			auto Execute(
				DImportRecord& Record,
				EImportRecordAction Action,
				const FMultiOutputExecutionOptions& Options) const
				-> FImportRecordActionResult override
			{
				FImportRecordActionResult Result;
				const bool bRecreate = Action != EImportRecordAction::Reimport;
				const FSceneImportPlanResult Planned =
					PlanSceneReimport(Record, bRecreate, Options.Progress);
				if (!Planned)
				{
					Result.Message = Planned.Message;
					Result.Diagnostics = Planned.Diagnostics;
					return Result;
				}
				FSceneImportExecutionResult Executed = ExecuteSceneImport(Planned.Plan, Options);
				Result.bSucceeded = Executed.bSucceeded;
				Result.Message = std::move(Executed.Message);
				Result.Record = Executed.Record;
				Result.Orphans = std::move(Executed.OrphanedAssets);
				Result.Diagnostics = std::move(Executed.Diagnostics);
				Result.Provider = std::move(Executed.Provider);
				for (DStaticMesh* Mesh : Executed.Meshes)
					Result.Outputs.push_back(Mesh);
				for (DMaterialInstance* Material : Executed.Materials)
					Result.Outputs.push_back(Material);
				for (DTexture2D* Texture : Executed.Textures)
					Result.Outputs.push_back(Texture);
				return Result;
			}
		};

		std::mutex GRegistrationMutex;
		bool GRegistered = false;
	}

	auto RegisterStandardAssetImportProviders(std::string& OutError) -> bool
	{
		std::lock_guard Lock(GRegistrationMutex);
		if (GRegistered) { OutError.clear(); return true; }
		AssetImport::OpenAsyncImportProviderAdmission(SceneImportProviderId);
		AssetImport::OpenAsyncImportProviderAdmission("Assimp");
		AssetImport::OpenAsyncImportProviderAdmission("DurinImage");
		if (!GStaticMeshSourceDecoderRegistered)
		{
			GStaticMeshSourceDecoderRegistered =
				RegisterStaticMeshSourceDecoder(&DecodeStaticMeshSource);
			if (!GStaticMeshSourceDecoderRegistered)
			{
				OutError = "Another StaticMesh source decoder is already registered.";
				return false;
			}
		}
		auto& Providers = AssetImport::GetProviderRegistry();
		auto& Handlers = AssetImport::GetSingleAssetHandlerRegistry();
		auto& RecordHandlers = AssetImport::GetImportRecordHandlerRegistry();
		if (!Providers.Register(CreateSceneImportProvider(), OutError)) return false;
		if (!RecordHandlers.Register(std::make_shared<FSceneRecordHandler>(), OutError))
		{
			Providers.Unregister(SceneImportProviderId);
			return false;
		}
		if (!Providers.Register(std::make_shared<FIdentityProvider>("Assimp", 3,
			std::vector<std::string>{
				".obj", ".fbx", ".gltf", ".glb", ".dae", ".3ds", ".ply", ".stl"}),
			OutError))
		{
			RecordHandlers.Unregister(SceneImportProviderId);
			Providers.Unregister(SceneImportProviderId);
			return false;
		}
		if (!Providers.Register(std::make_shared<FIdentityProvider>("DurinImage", 1,
			std::vector<std::string>{".png", ".jpg", ".jpeg", ".bmp", ".tga", ".hdr"}), OutError))
		{
			RecordHandlers.Unregister(SceneImportProviderId);
			Providers.Unregister("Assimp");
			Providers.Unregister(SceneImportProviderId);
			return false;
		}
		std::vector<std::shared_ptr<ISingleAssetImportHandler>> Builtins = {
			std::make_shared<FStaticMeshHandler>(),
			std::make_shared<FTexture2DHandler>(),
			std::make_shared<FTextureCubeHandler>()};
		for (const auto& Handler : Builtins)
		{
			if (Handlers.Register(Handler, OutError)) continue;
			Handlers.Unregister("Durin::DStaticMesh");
			Handlers.Unregister("Durin::DTexture2D");
			Handlers.Unregister("Durin::DTextureCube");
			RecordHandlers.Unregister(SceneImportProviderId);
			Providers.Unregister("DurinImage");
			Providers.Unregister("Assimp");
			Providers.Unregister(SceneImportProviderId);
			return false;
		}
		GRegistered = true;
		OutError.clear();
		return true;
	}

	auto UnregisterStandardAssetImportProviders() -> void
	{
		std::lock_guard Lock(GRegistrationMutex);
		if (!GRegistered) return;
		AssetImport::CancelAndDrainAsyncImportsForProvider(SceneImportProviderId);
		AssetImport::CancelAndDrainAsyncImportsForProvider("Assimp");
		AssetImport::CancelAndDrainAsyncImportsForProvider("DurinImage");
		auto& Handlers = AssetImport::GetSingleAssetHandlerRegistry();
		Handlers.Unregister("Durin::DStaticMesh");
		Handlers.Unregister("Durin::DTexture2D");
		Handlers.Unregister("Durin::DTextureCube");
		AssetImport::GetImportRecordHandlerRegistry().Unregister(SceneImportProviderId);
		auto& Providers = AssetImport::GetProviderRegistry();
		Providers.Unregister("DurinImage");
		Providers.Unregister("Assimp");
		Providers.Unregister(SceneImportProviderId);
		checkf(Providers.GetOutstandingLeaseCount("DurinImage") == 0
			&& Providers.GetOutstandingLeaseCount("Assimp") == 0
			&& Providers.GetOutstandingLeaseCount(SceneImportProviderId) == 0,
			"StandardAssetImport cannot unload while import plans, candidates, or results retain provider leases.");
		UnregisterStaticMeshSourceDecoder(&DecodeStaticMeshSource);
		GStaticMeshSourceDecoderRegistered = false;
		GRegistered = false;
	}
}
