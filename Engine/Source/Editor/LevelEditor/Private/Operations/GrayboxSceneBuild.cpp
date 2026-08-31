#include "GrayboxSceneBuild.h"

#include "Actors/DirectionalLightActor.h"
#include "Actors/PlayerStart.h"
#include "Actors/StaticMeshActor.h"
#include "Asset/AssetOperations.h"
#include "Asset/Mutation.h"
#include "Asset.h"
#include "Components/StaticMeshComponent.h"
#include "DObject/AssetPath.h"
#include "DObject/Package.h"
#include "Engine/Level.h"
#include "HAL/PlatformProcess.h"
#include "Math/Operations.h"
#include "Misc/Paths.h"
#include "Misc/MountPaths.h"
#include "Misc/Project.h"
#include "StaticMesh/StaticMesh.h"
#include "StaticMeshLevelMutations.h"

namespace Durin::Editor::Level
{
	namespace
	{
		constexpr double MinDimension = 0.1;
		constexpr double MaxDimension = 10000.0;
		constexpr std::string_view BoxAssetPath = "/Engine/Models/Box";

		auto IsValidDimension(double Value) -> bool
		{
			return std::isfinite(Value)
				&& Value >= MinDimension && Value <= MaxDimension;
		}

		auto ParseDouble(std::string_view Text, double& OutValue) -> bool
		{
			const auto [End, Error] = std::from_chars(
				Text.data(), Text.data() + Text.size(), OutValue);
			return Error == std::errc{} && End == Text.data() + Text.size()
				&& std::isfinite(OutValue);
		}

		auto MakeBoxTransform(
			const FBox& Bounds,
			const FVector3& DesiredCenter,
			const FVector3& DesiredSize) -> FTransform
		{
			const FVector3 LocalSize = Bounds.Max - Bounds.Min;
			const FVector3 LocalCenter = (Bounds.Min + Bounds.Max) * 0.5;
			FTransform Transform;
			Transform.Scale3D = DesiredSize / LocalSize;
			Transform.Translation = DesiredCenter - LocalCenter * Transform.Scale3D;
			return Transform;
		}

		auto SameTransform(const FTransform& A, const FTransform& B) -> bool
		{
			constexpr double Epsilon = 1e-6;
			auto Near = [](double Left, double Right) {
				return std::abs(Left - Right) <= Epsilon;
			};
			return Near(A.Translation.x, B.Translation.x)
				&& Near(A.Translation.y, B.Translation.y)
				&& Near(A.Translation.z, B.Translation.z)
				&& Near(A.Scale3D.x, B.Scale3D.x)
				&& Near(A.Scale3D.y, B.Scale3D.y)
				&& Near(A.Scale3D.z, B.Scale3D.z)
				&& Near(std::abs(
					A.Rotation.w * B.Rotation.w
					+ A.Rotation.x * B.Rotation.x
					+ A.Rotation.y * B.Rotation.y
					+ A.Rotation.z * B.Rotation.z), 1.0);
		}

		auto VerifyArena(
			DLevel& Level,
			DStaticMesh& Box,
			const FGrayboxOpenArenaLayout& Layout,
			std::string& OutError) -> bool
		{
			if (Level.GetActors().size() != Layout.Pieces.size() + 2)
			{
				OutError = "The reloaded Level has an unexpected Actor count.";
				return false;
			}
			for (const FGrayboxArenaPiece& Piece : Layout.Pieces)
			{
				auto* Actor = Cast<AStaticMeshActor>(Level.FindActorByName(Piece.Name));
				if (!Actor || !Actor->GetStaticMeshComponent()
					|| Actor->GetStaticMeshComponent()->GetStaticMesh() != &Box
					|| !SameTransform(Actor->GetActorTransform(), Piece.Transform))
				{
					OutError = std::format(
						"Reload verification failed for Actor '{}'.", Piece.Name.ToString());
					return false;
				}
			}
			auto* PlayerStart = Cast<APlayerStart>(Level.FindActorByName("PlayerStart"));
			auto* Light = Cast<ADirectionalLightActor>(
				Level.FindActorByName("DirectionalLight"));
			if (!PlayerStart || !Light
				|| !SameTransform(PlayerStart->GetActorTransform(), Layout.PlayerStartTransform)
				|| !SameTransform(Light->GetActorTransform(), Layout.DirectionalLightTransform))
			{
				OutError = "Reload verification failed for PlayerStart or DirectionalLight.";
				return false;
			}
			return true;
		}

		auto CleanupCandidate(const FPackagePath& Path) -> bool
		{
			if (DPackage* Resident = Asset::FindResidentPackage(Path))
				(void)Asset::UnloadPackage(
					Resident,
					Asset::EAssetPackageUnloadPolicy::DiscardUnsaved);
			if (Asset::FindAssetExact(Path))
			{
				Asset::FAssetDeletionTransaction Transaction;
				std::vector<Asset::FAssetDeletionBatchBlocker> Blockers;
				Asset::FAssetResult Result = Asset::PrepareAssetDeletionTransaction(
					std::span{&Path, 1}, {}, Transaction, Blockers);
				if (!Result || !Blockers.empty()) return false;

				std::vector<std::pair<std::filesystem::path, std::filesystem::path>> Moves;
				const uint64 OperationId = static_cast<uint64>(
					std::chrono::steady_clock::now().time_since_epoch().count());
				for (const Asset::FAssetDeletionBatchEntry& Entry :
					 Transaction.GetEntries())
				{
					std::vector<std::filesystem::path> Files{
						Entry.RegistryEntry.PhysicalPath};
					Files.insert(
						Files.end(), Entry.CompanionFiles.begin(), Entry.CompanionFiles.end());
					for (const std::filesystem::path& File : Files)
					{
						std::filesystem::path Staged = File;
						Staged += std::format(".durin-delete-{:016x}-{}", OperationId, Moves.size());
						std::error_code Error;
						if (std::filesystem::exists(Staged, Error) || Error) return false;
						Moves.emplace_back(File, std::move(Staged));
					}
				}

				auto MoveAll = [&Moves](bool bStage) -> Asset::FAssetResult {
					size_t Moved = 0;
					for (; Moved < Moves.size(); ++Moved)
					{
						const auto& [Original, Staged] = Moves[Moved];
						std::error_code Error;
						std::filesystem::rename(
							bStage ? Original : Staged,
							bStage ? Staged : Original,
							Error);
						if (!Error) continue;
						while (Moved > 0)
						{
							--Moved;
							const auto& [RollbackOriginal, RollbackStaged] = Moves[Moved];
							std::error_code RollbackError;
							std::filesystem::rename(
								bStage ? RollbackStaged : RollbackOriginal,
								bStage ? RollbackOriginal : RollbackStaged,
								RollbackError);
							if (RollbackError)
								return {Asset::EAssetError::IoError, std::format(
									"Candidate cleanup rollback failed: {}", RollbackError.message())};
						}
						return {Asset::EAssetError::IoError, std::format(
							"Candidate cleanup staging failed: {}", Error.message())};
					}
					return {};
				};
				Result = Transaction.Commit({
					.Stage = [&] { return MoveAll(true); },
					.Restore = [&] { return MoveAll(false); },
				});
				if (!Result) return false;
				for (const auto& [Original, Staged] : Moves)
				{
					(void)Original;
					std::error_code Error;
					if (!std::filesystem::remove(Staged, Error) || Error) return false;
				}
				return true;
			}
			const FAssetPathResult Resolved =
				FMountPaths::ResolveAssetPath(Path.ToString());
			if (!Resolved) return false;
			std::filesystem::path PackageFile = Resolved.PhysicalPath;
			PackageFile += ".dasset";
			std::error_code Error;
			if (!std::filesystem::exists(PackageFile, Error)) return !Error;
			return std::filesystem::remove(PackageFile, Error) && !Error;
		}
	}

	auto BuildGrayboxOpenArenaLayout(
		const FGrayboxOpenArenaParams& Params,
		const FBox& BoxLocalBounds,
		FGrayboxOpenArenaLayout& OutLayout,
		std::string& OutError) -> bool
	{
		OutLayout = {};
		OutError.clear();
		if (!IsValidDimension(Params.Width) || !IsValidDimension(Params.Depth)
			|| !IsValidDimension(Params.FloorThickness)
			|| !IsValidDimension(Params.WallHeight)
			|| !IsValidDimension(Params.WallThickness))
		{
			OutError = std::format(
				"Arena dimensions must be finite values in [{}, {}].",
				MinDimension, MaxDimension);
			return false;
		}
		if (!BoxLocalBounds.bIsValid)
		{
			OutError = "The Box mesh has no valid local bounds.";
			return false;
		}
		const FVector3 LocalSize = BoxLocalBounds.Max - BoxLocalBounds.Min;
		if (!IsValidDimension(LocalSize.x) || !IsValidDimension(LocalSize.y)
			|| !IsValidDimension(LocalSize.z))
		{
			OutError = "The Box mesh local bounds are non-finite or degenerate.";
			return false;
		}

		const double OuterWidth = Params.Width + 2.0 * Params.WallThickness;
		const double OuterDepth = Params.Depth + 2.0 * Params.WallThickness;
		const double SeamOverlap = std::min(
			Params.FloorThickness, Params.WallThickness) * 0.1;
		const double WallSpanZ = Params.WallHeight + SeamOverlap;
		const double WallCenterZ = (Params.WallHeight - SeamOverlap) * 0.5;
		auto Add = [&](std::string_view Name, FVector3 Center, FVector3 Size) {
			OutLayout.Pieces.push_back({
				FName(Name), MakeBoxTransform(BoxLocalBounds, Center, Size)});
		};
		Add("Graybox_Floor", {0.0, 0.0, -Params.FloorThickness * 0.5},
			{OuterWidth, OuterDepth, Params.FloorThickness});
		Add("Graybox_WallNorth",
			{0.0, Params.Depth * 0.5 + Params.WallThickness * 0.5, WallCenterZ},
			{OuterWidth, Params.WallThickness, WallSpanZ});
		Add("Graybox_WallSouth",
			{0.0, -Params.Depth * 0.5 - Params.WallThickness * 0.5, WallCenterZ},
			{OuterWidth, Params.WallThickness, WallSpanZ});
		Add("Graybox_WallEast",
			{Params.Width * 0.5 + Params.WallThickness * 0.5, 0.0, WallCenterZ},
			{Params.WallThickness, Params.Depth, WallSpanZ});
		Add("Graybox_WallWest",
			{-Params.Width * 0.5 - Params.WallThickness * 0.5, 0.0, WallCenterZ},
			{Params.WallThickness, Params.Depth, WallSpanZ});
		if (Params.bCeiling)
			Add("Graybox_Ceiling",
				{0.0, 0.0, Params.WallHeight + Params.FloorThickness * 0.5},
				{OuterWidth, OuterDepth, Params.FloorThickness});

		OutLayout.PlayerStartTransform.Translation = {0.0, 0.0, 0.1};
		OutLayout.DirectionalLightTransform.Rotation =
			Math::MakeQuaternionFromEulerDegrees({-45.0, -35.0, 0.0});
		return true;
	}

	auto RunGrayboxBuildStartupCommand(std::span<const std::string> Arguments) -> int
	{
		FGrayboxOpenArenaParams Params;
		std::string OutputText;
		std::string Preset = "open-arena";
		for (const std::string& Argument : Arguments)
		{
			auto ReadValue = [&](std::string_view Prefix, double& Value) {
				return Argument.starts_with(Prefix)
					&& ParseDouble(std::string_view(Argument).substr(Prefix.size()), Value);
			};
			if (Argument.starts_with("--output="))
				OutputText = Argument.substr(std::string_view("--output=").size());
			else if (Argument.starts_with("--preset="))
				Preset = Argument.substr(std::string_view("--preset=").size());
			else if (Argument == "--ceiling") Params.bCeiling = true;
			else if (ReadValue("--width=", Params.Width)
				|| ReadValue("--depth=", Params.Depth)
				|| ReadValue("--floor-thickness=", Params.FloorThickness)
				|| ReadValue("--wall-height=", Params.WallHeight)
				|| ReadValue("--wall-thickness=", Params.WallThickness)) {}
			else
			{
				DURIN_ERROR("graybox-build: invalid argument '{}'.", Argument);
				return 2;
			}
		}
		if (Preset != "open-arena" || OutputText.empty())
		{
			DURIN_ERROR("graybox-build requires --output and preset 'open-arena'.");
			return 2;
		}
		const FProjectInfo* Project = GetCurrentProject();
		FPackagePath OutputPath;
		if (!Project || !FPackagePath::TryCreate(OutputText, OutputPath)
			|| !OutputPath.ToString().starts_with(Project->MountRoot))
		{
			DURIN_ERROR("graybox-build: output must be a valid path in the current project mount.");
			return 2;
		}
		if (Asset::FindAssetExact(OutputPath)
			|| Asset::FindResidentPackage(OutputPath))
		{
			DURIN_ERROR("graybox-build: output '{}' already exists; replacement is not supported.", OutputText);
			return 3;
		}

		FPackagePath BoxPath;
		check(FPackagePath::TryCreate(BoxAssetPath, BoxPath));
		DStaticMesh* Box = nullptr;
		const Asset::FAssetResult BoxResult = Asset::LoadAsset(BoxPath, Box);
		if (!BoxResult || !Box)
		{
			DURIN_ERROR("graybox-build: could not load {}: {}", BoxAssetPath, BoxResult.Message);
			return 4;
		}
		const std::optional<FBox> BoxBounds = Box->GetLOD0LocalBounds();
		FGrayboxOpenArenaLayout Layout;
		std::string Error;
		if (!BoxBounds || !BuildGrayboxOpenArenaLayout(Params, *BoxBounds, Layout, Error))
		{
			DURIN_ERROR("graybox-build: {}", Error.empty() ? "Box bounds are unavailable." : Error);
			return 4;
		}

		FPackagePath CandidatePath;
		const size_t Slash = OutputText.find_last_of('/');
		const std::string Directory = OutputText.substr(0, Slash + 1);
		bool bCandidatePathFound = false;
		for (uint32 Attempt = 0; Attempt < 100; ++Attempt)
		{
			const std::string Text = std::format(
				"{}__GrayboxBuildCandidate_{}_{}", Directory,
				FPlatformProcess::CurrentProcessId(), Attempt);
			if (FPackagePath::TryCreate(Text, CandidatePath)
				&& !Asset::FindAssetExact(CandidatePath)
				&& !Asset::FindResidentPackage(CandidatePath))
			{
				bCandidatePathFound = true;
				break;
			}
		}
		if (!bCandidatePathFound)
		{
			DURIN_ERROR("graybox-build: could not reserve a temporary candidate path.");
			return 3;
		}

		DLevel* Candidate = nullptr;
		Asset::FAssetResult Result = Asset::CreateAsset(CandidatePath, Candidate);
		if (!Result || !Candidate)
		{
			DURIN_ERROR("graybox-build: could not create candidate: {}", Result.Message);
			return 5;
		}
		auto FailCandidate = [&](int Code, std::string_view Message) {
			const bool bClean = CleanupCandidate(CandidatePath);
			DURIN_ERROR("graybox-build: {}{}", Message,
				bClean ? "" : " Candidate cleanup also failed.");
			return Code;
		};

		FStaticMeshLevelMutationRequest Request =
			FStaticMeshLevelMutations::CaptureTarget(*Candidate);
		Request.Description = "Build open graybox arena";
		for (const FGrayboxArenaPiece& Piece : Layout.Pieces)
			Request.Mutations.push_back({
				.Kind = EStaticMeshLevelMutationKind::Create,
				.TargetName = Piece.Name,
				.Desired = {
					.Name = Piece.Name,
					.StaticMesh = Box,
					.Transform = Piece.Transform}});
		const FStaticMeshLevelMutationPlan Plan =
			FStaticMeshLevelMutations::Plan(Request);
		const FStaticMeshLevelMutationResult MutationResult =
			FStaticMeshLevelMutations::Execute(Plan, {.OpenLevel = Candidate});
		if (!MutationResult)
			return FailCandidate(5, MutationResult.Diagnostic.Message);

		APlayerStart* PlayerStart = Candidate->SpawnActor<APlayerStart>("PlayerStart");
		ADirectionalLightActor* Light =
			Candidate->SpawnActor<ADirectionalLightActor>("DirectionalLight");
		if (!PlayerStart || !Light
			|| !PlayerStart->SetActorTransform(Layout.PlayerStartTransform)
			|| !Light->SetActorTransform(Layout.DirectionalLightTransform))
			return FailCandidate(5, "could not create the baseline gameplay Actors.");
		Result = Asset::SavePackage(Candidate->GetPackage());
		if (!Result) return FailCandidate(6, Result.Message);
		Result = Asset::UnloadPackage(CandidatePath);
		if (!Result) return FailCandidate(6, Result.Message);
		Candidate = nullptr;
		Result = Asset::LoadAsset(CandidatePath, Candidate);
		if (!Result || !Candidate || !VerifyArena(*Candidate, *Box, Layout, Error))
			return FailCandidate(7, Error.empty() ? Result.Message : Error);
		Result = Asset::UnloadPackage(CandidatePath);
		if (!Result) return FailCandidate(6, Result.Message);
		Candidate = nullptr;

		Asset::FAssetMutationSummary Summary;
		Asset::FAssetMutationTransaction Transaction;
		const Asset::FAssetRelocationMapping Mapping{CandidatePath, OutputPath};
		Result = Asset::PrepareAssetRelocationTransaction(
			std::span{&Mapping, 1}, Summary, Transaction);
		if (Result) Result = Transaction.Commit();
		if (!Result) return FailCandidate(6, Result.Message);

		DLevel* Published = nullptr;
		Result = Asset::LoadAsset(OutputPath, Published);
		if (!Result || !Published || !VerifyArena(*Published, *Box, Layout, Error))
		{
			if (DPackage* Resident = Asset::FindResidentPackage(OutputPath))
				(void)Asset::UnloadPackage(
					Resident,
					Asset::EAssetPackageUnloadPolicy::DiscardUnsaved);
			const Asset::FAssetResult Restore = Transaction.Undo();
			CleanupCandidate(CandidatePath);
			DURIN_ERROR("graybox-build: published verification failed: {}{}",
				Error.empty() ? Result.Message : Error,
				Restore ? "" : std::format("; restore failed: {}", Restore.Message));
			return 7;
		}
		Asset::UnloadPackage(OutputPath);
		DURIN_INFO("graybox-build: created open{} arena '{}' with {} Box Actors.",
			Params.bCeiling ? "-ceiling" : "-air", OutputText, Layout.Pieces.size());
		return 0;
	}
}
