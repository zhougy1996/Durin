#include "RendererModule.h"
#include "CoreMinimal.h"
#include "Misc/UnifiedError.h"
#include "WaterSurfaceActor.h"
#include "WaterSurfaceComponent.h"
#include "WaterSurfaceResources.h"
#include "WaterMaterialRecipe.h"
#include "CoreGlobals.h"
#include "HAL/PlatformLTS.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/Package.h"
#include "DObject/Class.h"
#include "DObject/Property.h"
#include "Engine/Level.h"
#include "Interfaces/IProjectManager.h"
#include "Modules/ModuleManager.h"
#include "Misc/MountPaths.h"
#include "Math/Operations.h"
#include "Asset/Load.h"
#include "Asset/AssetCompilingManager.h"
#include "Asset/PackageSerialization.h"
#include "Asset/PackageCollection.h"
#include "AssetRegistry/Scan.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Shader/ShaderData.h"
#include "Materials/MaterialInterface.h"
#include "Threading/TaskRuntime.h"
#include "AuthorWaterMaterial.h"

#if DURIN_WITH_EDITOR
#include "Actors/CameraActor.h"
#include "Actors/DirectionalLightActor.h"
#include "Actors/PlayerStart.h"
#include "Actors/SkyLightActor.h"
#include "Actors/StaticMeshActor.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Math/Transform.h"
#include "StaticMesh/StaticMesh.h"
#include "Texture/TextureCube.h"
#endif

namespace
{
	using namespace Durin;
	auto Require(bool Value, std::string_view Message) -> void { if (!Value) throw std::runtime_error(std::string(Message)); }
	auto Length(FVector3 V) -> double { return std::sqrt(Math::Dot(V, V)); }

#if DURIN_WITH_EDITOR
	auto AuthorWaterDemoLevel() -> void
	{
		Require(bool(RefreshAssetRegistry()), "Asset registry scan failed.");
		FSoftObjectPath BoxPath, SkyPath;
		Require(FSoftObjectPath::TryCreate("/Engine/Models/Box.Box", BoxPath), "Invalid box path.");
		Require(FSoftObjectPath::TryCreate("/Engine/Renderer/DefaultStudioCube.DefaultStudioCube", SkyPath), "Invalid sky path.");
		std::optional<UnifiedError::FError> AssetError;
		auto* Box = LoadObject<DStaticMesh>(BoxPath, nullptr, &AssetError);
		Require(Box && Box->GetNormalizedSize() > 0.f, AssetError ? AssetError->ToString() : "Demo box is unavailable.");
		auto* Sky = LoadObject<DTextureCube>(SkyPath, nullptr, &AssetError);
		Require(Sky != nullptr, AssetError ? AssetError->ToString() : "Demo studio sky is unavailable.");

		FPackagePath Path;
		Require(FPackagePath::TryCreate("/Game/Levels/SingleLayerWaterDemo", Path), "Invalid demo level path.");
		auto* Package = CreatePackage(Path);
		auto* Level = NewObject<DLevel>(Package, "SingleLayerWaterDemo", EObjectFlags::Public);
		const auto Place = [](AActor* Actor, const FVector3& Position, const FVector3& Angles = {},
			const FVector3& Scale = FVector3(1.0)) {
			FTransform Transform;
			Transform.Translation = Position;
			Transform.Rotation = Math::MakeQuaternionFromEulerDegrees(Angles);
			Transform.Scale3D = Scale;
			Require(Actor && Actor->SetActorTransform(Transform), "Demo actor placement failed.");
		};
		const auto AddBox = [&](FName Name, const FVector3& Center, const FVector3& Dimensions) {
			auto* Actor = Level->SpawnActor<AStaticMeshActor>(Name);
			Place(Actor, Center, {}, Dimensions / double(Box->GetNormalizedSize()));
			Actor->GetStaticMeshComponent()->SetStaticMesh(Box);
		};

		auto* Water = Level->SpawnActor<AWaterSurfaceActor>("Water");
		Require(Water && Water->GetWaterComponent(), "Water actor construction failed.");
		auto Settings = Water->GetWaterComponent()->GetSettings();
		Settings.Extent = {16., 12.};
		Settings.Amplitude = .12f;
		Require(bool(Water->GetWaterComponent()->SetSettings(Settings)), "Demo water settings rejected.");

		// Tops of the three beds sit at 25, 100 and 200 cm below the still surface.
		const double Width = Settings.Extent.x / 3.0;
		AddBox("Bed025cm", {-Width, 0, -.4}, {Width, 12., .3});
		AddBox("Bed100cm", {0, 0, -1.15}, {Width, 12., .3});
		AddBox("Bed200cm", {Width, 0, -2.15}, {Width, 12., .3});
		AddBox("PierShallow", {-4., 2., .35}, {.65, .65, 1.5});
		AddBox("PierDeep", {4., -2., -.45}, {.75, .75, 3.3});
		AddBox("ObservationDeck", {-10., 0., -.15}, {4., 5., .6});
		Place(Level->SpawnActor<APlayerStart>("PlayerStart"), {-10.5, 0., 1.5});
		Place(Level->SpawnActor<ACameraActor>("WaterOverview"), {-12., 0., 7.}, {0., 27., 0.});
		auto* Sun = Level->SpawnActor<ADirectionalLightActor>("Sun");
		Place(Sun, {0., 0., 5.}, {0., 52., -30.});
		Sun->GetLightComponent()->SetIntensity(3.f);
		auto* SkyLight = Level->SpawnActor<ASkyLightActor>("StudioSky");
		Require(SkyLight != nullptr, "Demo sky light construction failed.");
		SkyLight->GetSkyLightComponent()->SetSource(ESkyLightSourceMode::SpecifiedCube, Sky);

		const auto ActorCount = Level->GetActors().size();
		Require(bool(SavePackage(Package)), "Demo level save failed.");
		const auto Unloaded = CollectPackageExports(std::array{TWeakObjectPtr<DPackage>(Package)});
		Require(bool(Unloaded.Admission) && Unloaded.CollectedCount > 0, "Demo level unload failed.");
		const auto Loaded = LoadPackage(Path);
		auto* Reloaded = Loaded ? Cast<DLevel>(Loaded->FindTopLevelAsset("SingleLayerWaterDemo")) : nullptr;
		Require(Reloaded && Reloaded->GetActors().size() == ActorCount, "Demo level actors did not reopen.");
		auto* ReloadedWater = Cast<AWaterSurfaceActor>(Reloaded->FindActorByName("Water"));
		Require(ReloadedWater && ReloadedWater->GetWaterComponent()->GetSettings() == Settings, "Demo wave settings did not persist.");
		std::cout << "SingleLayerWaterDemo saved and reopened with " << ActorCount << " actors\n";
	}
#endif

	// Independent Float32 evaluation of the material recipe, including its normal
	// decode. It does not call the CPU wave evaluator or inverse query.
	auto EvaluateMaterial(const MIR::FCompilerInput& Input, const FWaterSurfaceSettings& S, FVector2 P, double Time)
		-> std::pair<FVector3, FVector3>
	{
		std::vector<FVector4f> V;
		const std::array Parameters{S.Amplitude, S.Wavelength, S.Speed, S.Steepness};
		for (const auto& N : Input.IR.Nodes)
		{
			auto A = [&](uint32 I) -> FVector4f { return V.at(N.Inputs.at(I)); };
			FVector4f R(0);
			switch (N.Opcode)
			{
			case EMaterialProgramOpcode::Constant: { const auto C = N.GetLiteral(); R = {C.X, C.Y, C.Z, C.W}; break; }
			case EMaterialProgramOpcode::UVChannel: R = {float(P.x), float(P.y), 0, 0}; break;
			case EMaterialProgramOpcode::Time: R.x = float(Time); break;
			case EMaterialProgramOpcode::PrimitiveCustomData: R.x = Parameters.at(std::get<MIR::FCustomDataRead>(N.Payload).Index); break;
			case EMaterialProgramOpcode::Swizzle: { const auto Sw = N.GetSwizzle(); for (uint8 I = 0; I < Sw.Length; ++I) R[I] = A(0)[Sw.Components[I]]; break; }
			case EMaterialProgramOpcode::Splat3: R = FVector4f(A(0).x); break;
			case EMaterialProgramOpcode::MakeFloat3: R = {A(0).x, A(1).x, A(2).x, 0}; break;
			case EMaterialProgramOpcode::Multiply: R = A(0) * A(1); break;
			case EMaterialProgramOpcode::Divide: R.x = A(0).x / A(1).x; break;
			case EMaterialProgramOpcode::Add: R = A(0) + A(1); break;
			case EMaterialProgramOpcode::Subtract: R = A(0) - A(1); break;
			case EMaterialProgramOpcode::Sqrt: R.x = std::sqrt(A(0).x); break;
			case EMaterialProgramOpcode::Sine: R.x = std::sin(A(0).x); break;
			case EMaterialProgramOpcode::Cosine: R.x = std::cos(A(0).x); break;
			case EMaterialProgramOpcode::Cross: R = FVector4f(Math::Cross(FVector3f(A(0)), FVector3f(A(1))), 0); break;
			case EMaterialProgramOpcode::Normalize: R = FVector4f(Math::Normalize(FVector3f(A(0))), 0); break;
			case EMaterialProgramOpcode::TransformDirection: R = A(0); break; // Identity component transform in this comparison.
			case EMaterialProgramOpcode::DecodeNormalRG: {
				const auto XY = FVector2f(A(0)) * 2.f - FVector2f(1);
				R = FVector4f(XY, std::sqrt(std::max(0.f, 1.f - Math::Dot(XY, XY))), 0); break;
			}
			default: throw std::runtime_error("Unhandled Water material operation in independent evaluator.");
			}
			V.push_back(R);
		}
		const auto Offset = FVector3(V.at(Input.IR.GetAttribute(EMaterialProperty::MP_WorldPositionOffset).ExpressionIndex));
		const auto Normal = FVector3(V.at(Input.IR.GetAttribute(EMaterialProperty::MP_Normal).ExpressionIndex));
		return {FVector3(P, S.Elevation) + Offset, Normal};
	}

	auto CheckQueries(const MIR::FCompilerInput& Input = WaterMaterialRecipe::BuildInput()) -> void
	{
		std::array<FWaterSurfaceSettings, 4> Cases;
		Cases[1].Amplitude = 0;
		Cases[2].Extent = {128, 128}; Cases[2].Wavelength = 64; Cases[2].Amplitude = 2; Cases[2].Steepness = 1;
		Cases[3].Extent = {8, 8}; Cases[3].Wavelength = 2; Cases[3].Amplitude = .29f; Cases[3].Steepness = .5f; Cases[3].Speed = 4;
		for (const auto& S : Cases)
		{
			Require(bool(ValidateWaterSurface(S)), "Valid water settings rejected.");
			for (double T : {0., .75, 17., 3599.}) for (const FVector2 Fraction : {FVector2(0), FVector2(.43, -.39), FVector2(-.45, .45)})
			{
				const auto P = S.Extent * Fraction;
				const auto Forward = EvaluateWaterSurface(S, P, T);
				const auto Query = QueryWaterSurface(S, FVector2(Forward.Position), T);
				Require(Query && Length(Query->Position - Forward.Position) < 1.e-7, "Gerstner inverse failed.");
				Require(Length(Query->Normal - Forward.Normal) < 1.e-7, "Inverse normal disagrees.");
				const double H = 1.e-4;
				const auto TX = EvaluateWaterSurface(S, P + FVector2(H, 0), T).Position - EvaluateWaterSurface(S, P - FVector2(H, 0), T).Position;
				const auto TY = EvaluateWaterSurface(S, P + FVector2(0, H), T).Position - EvaluateWaterSurface(S, P - FVector2(0, H), T).Position;
				Require(Length(Math::Normalize(Math::Cross(TX, TY)) - Forward.Normal) < 1.e-6, "Analytic normal disagrees with finite differences.");
				const auto [GPU, Normal] = EvaluateMaterial(Input, S, P, T);
				Require(Length(GPU - Forward.Position) < .005, "Material/CPU position exceeds 5 mm tolerance.");
				Require(Length(Normal - Forward.Normal) < .02, "Material/CPU normal exceeds 0.02 vector tolerance.");
			}
			Require(!QueryWaterSurface(S, S.Extent * 2., 0), "Outside extent query returned water.");
			Require(!QueryWaterSurface(S, {0, 0}, std::numeric_limits<double>::infinity()), "Nonfinite time accepted.");
			Require(!QueryWaterSurface(S, {0, 0}, std::numeric_limits<double>::max()), "Overflowing phase returned a nonfinite surface.");
			auto Grid = MakeWaterGrid(S);
			Require(Grid && Grid->LODResources[0].GetNumVertices() <= 257 * 257, "Grid allocation bound exceeded.");
			const auto UVs = Grid->LODResources[0].VertexBuffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.GetTexCoords()[0];
			const auto Indices = Grid->LODResources[0].IndexBuffer.GetIndices();
			for (size_t I = 0; I + 2 < Indices.size(); I += 3 * std::max<size_t>(1, Indices.size() / 300))
			{
				FVector3 TrianglePoint(0);
				for (uint32 J = 0; J < 3; ++J) TrianglePoint += EvaluateWaterSurface(S, FVector2(UVs[Indices[I + J]]), .75).Position / 3.;
				const auto Q = QueryWaterSurface(S, FVector2(TrianglePoint), .75);
				Require(Q && std::abs(Q->Position.z - TrianglePoint.z) < .02, "Rendered triangle/query height exceeds 2 cm tessellation tolerance.");
			}
		}
		auto Invalid = Cases[0]; Invalid.Amplitude = std::numeric_limits<float>::quiet_NaN();
		Require(!ValidateWaterSurface(Invalid), "Nonfinite settings accepted.");
		Invalid = Cases[0]; Invalid.Wavelength = 2; Invalid.Amplitude = 2; Invalid.Steepness = 1;
		Require(!ValidateWaterSurface(Invalid), "Overturning waves accepted.");
		Invalid = Cases[0]; Invalid.Extent = {128, 128}; Invalid.Wavelength = 2;
		Require(!ValidateWaterSurface(Invalid), "Unbounded grid accepted.");
		auto* Component = NewObject<DWaterSurfaceComponent>(nullptr, NAME_None);
		auto S = Cases[0]; S.Elevation = 3;
		Require(bool(Component->SetSettings(S)), "Component settings failed.");
		Require(Component->QuerySurface({0, 0, 0}, .75).has_value(), "Component query failed.");
		S.Amplitude = 0; S.Elevation = 7;
		Require(bool(Component->SetSettings(S)), "Parameter edit failed.");
		Require(std::abs(Component->QuerySurface({0, 0, 0}, .75)->Position.z - 7) < 1.e-8, "Query retained stale edited settings.");
		Require(!Component->SetSettings(Invalid), "Invalid edit accepted.");
		Require(Component->GetSettings() == S, "Rejected edit mutated settings.");
		const auto* Property = Component->GetClass()->FindPropertyByName("Settings");
		Require(Property != nullptr, "Water settings are not reflected.");
		auto* Draft = NewObject<DWaterSurfaceComponent>(nullptr, NAME_None);
		*Property->ContainerPtrToValuePtr<FWaterSurfaceSettings>(Draft) = Invalid;
		FPropertyEditProposal Proposal;
		Proposal.MemberProperty = Proposal.LeafProperty = Proposal.DraftRootProperty = Property;
		Proposal.DraftRootContainer = Draft;
		Require(!Component->PreEditChangeProperty(Proposal), "Invalid reflected edit proposal accepted.");
		*Property->ContainerPtrToValuePtr<FWaterSurfaceSettings>(Draft) = S;
		Require(bool(Component->PreEditChangeProperty(Proposal)), "Valid reflected edit proposal rejected.");
		Require(Component->GetSettings() == S, "Edit proposal mutated the live component.");
		FTransform Transform;
		Transform.Translation = {5, 8, 9}; Transform.Scale3D = FVector3(2);
		Transform.Rotation = Math::MakeQuaternionFromAxisAngleRadians(.4, FVector3(0, 1, 0));
		Component->SetRelativeTransform(Transform);
		const auto Matrix = Transform.ToMatrix();
		const auto WorldQuery = Component->QuerySurface(FVector3(Matrix * FVector4(0, 0, 0, 1)), .75);
		Require(WorldQuery && Length(WorldQuery->Position - FVector3(Matrix * FVector4(0, 0, 7, 1))) < 1.e-7,
			"World query projection disagrees with the component transform.");
		Require(Length(WorldQuery->Normal - Math::RotateVector(Transform.Rotation, FVector3(0, 0, 1))) < 1.e-7,
			"World normal disagrees with the component transform.");
		Transform.Scale3D.y = 3; Component->SetRelativeTransform(Transform);
		Require(!Component->QuerySurface({0, 0, 0}, 0), "Unsupported nonuniform transform produced a query surface.");
		std::cout << "Water queries: inversion, normals, material agreement, bounds, edits passed\n";
	}
}

int main(int Argc, char** Argv)
{
	using namespace Durin;
	FRendererModule ShaderInventory;
	if (Argc < 3) return 2;
	GGameThreadId = FPlatformLTS::GetCurrentThreadId(); GIsGameThreadIdInitialized = true; FNameInit();
	InitializeTaskScheduler(2);
	auto& Projects = IProjectManager::Get(); auto& Modules = FModuleManager::Get();
	try
	{
		Require(Projects.LoadProjectFile(Argv[1]) && Projects.PreparePluginStartup(), Projects.GetLastError());
		std::string Error;
		Require(FMountPaths::InitDefaultMountPoints(&Error), Error);
		const std::string Mode = Argv[2];
		const bool Cooked = Mode == "load-cooked";
		auto Configuration = FAssetRuntimeConfiguration::Authored();
		if (Cooked)
		{
			Require(Argc >= 4, "Expected absolute Cook directory.");
			Require(bool(FAssetRuntimeConfiguration::Cooked(Argv[3], Configuration)), "Invalid Cook directory.");
			Require(bool(InitializeShaderData(FShaderDataConfiguration::Cooked(Argv[3]))), "Cooked Shader library initialization failed.");
		}
		Require(bool(InitializeAssetRuntime(std::move(Configuration))), "Asset runtime initialization failed.");
		DObjectInit();
		const bool NeedsAssetCompilers = Cooked || Mode == "author-demo-level";
		if (NeedsAssetCompilers) InitializeAssetCompilingManager();
		Require(Projects.LoadModulesForProject(ELoadingPhase::Providers), Projects.GetLastError());
		if (Mode == "queries") CheckQueries();
		else if (Mode == "load" || Mode == "load-cooked")
		{
			std::optional<Durin::UnifiedError::FError> LoadedError;
			if (Mode == "load") Require(bool(RefreshAssetRegistry()), "Asset registry scan failed.");
			FPackagePath Path; Require(FPackagePath::TryCreate("/Game/WaterSurfaceLevel", Path), "Invalid level path.");
			const auto Loaded = LoadPackage(Path, nullptr, &LoadedError);
			Require(bool(Loaded), Loaded ? "" : LoadedError->ToString());
			auto* Level = Cast<DLevel>(Loaded->FindTopLevelAsset("WaterSurfaceLevel"));
			Require(Level && Level->GetActors().size() == 1, "Saved water level did not load.");
			auto* Actor = Cast<AWaterSurfaceActor>(Level->GetActors()[0].Get());
			Require(Actor && Actor->GetRootComponent() == Actor->GetWaterComponent(), "Saved water actor composition changed.");
			auto Expected = FWaterSurfaceSettings{}; Expected.Extent = {24, 20}; Expected.Elevation = 2.5f; Expected.Amplitude = .2f;
			Require(Actor->GetWaterComponent()->GetSettings() == Expected, "Saved water parameters changed across the runtime boundary.");
			Require(Actor->GetWaterComponent()->QuerySurface({0, 0, 0}, .75).has_value(), "Saved water query failed.");
			if (Cooked)
			{
				std::optional<Durin::UnifiedError::FError> MaterialError;
				FSoftObjectPath MaterialPath;
				Require(FSoftObjectPath::TryCreate("/Plugins/Water/Materials/DefaultWater.DefaultWater", MaterialPath), "Invalid material path.");
				const auto Material = LoadObject<DMaterialInterface>(MaterialPath, nullptr, &MaterialError);
				Require(bool(Material), Material ? "" : MaterialError->ToString());
				(void)FAssetCompilingManager::Get().FinishCompilationForObject(*Material);
				Require(bool(Material->GetAcceptedCompiledProgram()), "Cooked Water material has no accepted platform program.");
				Require(Material->GetStaticProperties().ShadingModel == EMaterialShadingModel::SingleLayerWater,
					"Cooked Water material has the wrong shading model.");
			}
			std::cout << "Saved water level and queries loaded in " << DURIN_RUNTIME_VARIANT << '\n';
		}
#if DURIN_WITH_EDITOR
		else if (Mode == "material")
		{
			std::optional<Durin::UnifiedError::FError> LoadedError;
			Require(bool(RefreshAssetRegistry()), "Asset registry scan failed.");
			FSoftObjectPath Path;
			Require(FSoftObjectPath::TryCreate("/Plugins/Water/Materials/DefaultWater.DefaultWater", Path), "Invalid material path.");
			const auto Loaded = LoadObject<DMaterial>(Path, nullptr, &LoadedError);
			Require(bool(Loaded), Loaded ? "" : LoadedError->ToString());
			const auto Snapshot = SnapshotMaterialCompilerInput(*Loaded, {});
			Require(bool(Snapshot), FormatMaterialProgramDiagnostics(Snapshot.Diagnostics));
			const auto Recipe = WaterMaterialRecipe::BuildInput();
			const auto& Input = Snapshot.Snapshot->Input;
			Require(Input.StaticProperties == Recipe.StaticProperties, "Saved Water material static properties differ from the recipe.");
			Require(Input.IR.GetAttribute(EMaterialProperty::MP_Opacity).Literal.X == 0.f, "Saved Water opacity changed.");
			for (const auto& Definition : FMaterialAttributeDefinitionMap::GetWaterOutputDefinitions())
			{
				const auto& Actual = Input.IR.GetAttribute(Definition.Property);
				const auto& Expected = Recipe.IR.GetAttribute(Definition.Property);
				Require(Actual.bExpression && Input.IR.Nodes.at(Actual.ExpressionIndex).GetLiteral()
					== Recipe.IR.Nodes.at(Expected.ExpressionIndex).GetLiteral(), "Saved Water optical coefficients differ from the recipe.");
			}
			CheckQueries(Snapshot.Snapshot->Input);
		}
		else if (Mode == "author-material")
		{
			Require(Argc == 4, "Expected source material output path.");
			AuthorWaterMaterial(Argv[3]);
			std::cout << "Authored Water source material (no compiled platform data)\n";
		}
		else if (Mode == "author-demo-level")
		{
			Modules.LoadModuleChecked("MeshBuilder");
			Modules.LoadModuleChecked("TextureCompressor");
			Modules.LoadModuleChecked("AssetForgeBuiltins");
			AuthorWaterDemoLevel();
		}
		else if (Mode == "persist")
		{
			std::optional<Durin::UnifiedError::FError> LoadedError;
			FPackagePath Path; Require(FPackagePath::TryCreate("/Game/WaterSurfaceLevel", Path), "Invalid level path.");
			auto* Package = CreatePackage(Path);
			auto* Level = NewObject<DLevel>(Package, "WaterSurfaceLevel", EObjectFlags::Public);
			auto* Water = Level->SpawnActor<AWaterSurfaceActor>("Water");
			Require(Water && Water->GetWaterComponent(), "Water actor construction failed.");
			auto S = Water->GetWaterComponent()->GetSettings(); S.Extent = {24, 20}; S.Elevation = 2.5f; S.Amplitude = .2f;
			Require(bool(Water->GetWaterComponent()->SetSettings(S)), "Settings mutation failed.");
			Require(bool(SavePackage(Package)), "Level save failed.");
			const auto Unloaded = CollectPackageExports(std::array{TWeakObjectPtr<DPackage>(Package)});
			Require(bool(Unloaded.Admission) && Unloaded.CollectedCount > 0, "Level unload failed.");
			const auto Loaded = LoadPackage(Path, nullptr, &LoadedError);
			Require(bool(Loaded), Loaded ? "" : LoadedError->ToString());
			auto* Reloaded = Cast<DLevel>(Loaded->FindTopLevelAsset("WaterSurfaceLevel"));
			Require(Reloaded && Reloaded->GetActors().size() == 1, "Level actor did not reopen.");
			auto* Actor = Cast<AWaterSurfaceActor>(Reloaded->GetActors()[0].Get());
			Require(Actor && Actor->GetWaterComponent()->GetSettings() == S, "Water settings did not persist.");
			Require(Actor->GetRootComponent() == Actor->GetWaterComponent(), "Actor component ownership changed after load.");
			std::cout << "Water level save/reopen passed\n";
		}
#endif
		else throw std::runtime_error("Unknown water probe mode.");
		if (Cooked) Require(!Modules.IsModuleLoaded("ShaderBuild"), "Cooked content loaded a compiler service.");
		if (NeedsAssetCompilers) ShutdownAssetCompilingManager();
		ShutdownAssetRuntime(); Modules.ShutdownModulesAtExit(); ShutdownTaskScheduler();
		return 0;
	}
	catch (const std::exception& Error) { std::cerr << Error.what() << '\n'; return 1; }
}
