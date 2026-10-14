#include "Physics/PhysicsCookHelper.h"
#include "PhysicsCookInputPrivate.h"
#if DURIN_WITH_EDITOR
#include "DerivedDataBuildSession.h"
#include "Asset/ResourceBuildSessions.h"
#include "PhysicsCookDerivedDataKey.h"
#include "PhysicsBuildFunction.h"
#include "PhysicsSharedOutput.h"
#endif

namespace Durin
{
#if DURIN_WITH_EDITOR
	namespace
	{
		AssetPrivate::FResourceBuildSessions PhysicsSessions;

		auto BuildPhysicsDefinition(DerivedData::FBuildDefinition Definition,
			std::shared_ptr<const DerivedData::IBuildInputResolver> Resolver,
			DerivedData::FBuildRequestOptions Options)
			-> std::expected<DerivedData::FBuildCompleteParams, DerivedData::FBuildAdmissionError>
		{
			static std::once_flag RegisterOnce;
			std::call_once(RegisterOnce, [] {
				if (!DerivedData::GetBuild().Register(PhysicsPrivate::MakeCollisionBuildFunction()))
					throw std::runtime_error("Failed to register the physics derived-data build function.");
			});
			return PhysicsSessions.Build(std::move(Definition), std::move(Resolver), std::move(Options));
		}
	}
	namespace PhysicsPrivate
	{
		auto ShutdownBuildSessions() -> void
		{ require(PhysicsSessions.Close() == DerivedData::EBuildDrainResult::Drained); }
	}
#endif

	auto FPhysicsCookHelper::Capture(FCookBodySetupInfo Info, const FAssetBuildTaskContext& Control)
		-> std::expected<FPhysicsCookInput, FPhysicsCookFailure>
	{
		if (Control.IsCancelled()) return {};
		FPhysicsCookInput Input;
		Input.Mode = Info.Mode; Input.Policy = Info.Policy; Input.bPersist = Info.bPersistDerivedData;
		if (Info.Mode == EBodySetupCollisionSourceMode::None) return Input;
		if (auto Valid = PhysicsPrivate::CheckCookInput(Info.TriangleMeshDesc.Positions.size(), Info.TriangleMeshDesc.Indices.size(),
			Info.Mode, Info.Policy, Control.MaximumWorkingSetBytes); !Valid) return std::unexpected(std::move(Valid.error()));
		if (!PhysicsPrivate::CheckCookReservation(Info.TriangleMeshDesc.Positions.capacity(), Info.TriangleMeshDesc.Indices.capacity(), Control.MaximumWorkingSetBytes))
			return std::unexpected(FPhysicsCookFailure{"Physics cook input and working set exceed the reservation."});
		Input.Positions = FSharedByteBuffer::TakeNative(std::move(Info.TriangleMeshDesc.Positions));
		Input.Indices = FSharedByteBuffer::TakeNative(std::move(Info.TriangleMeshDesc.Indices));
		auto Identity = PhysicsPrivate::HashInput(Input.Positions.GetBytes(), Input.Indices.GetBytes(), [&] { return Control.IsCancelled(); });
		if (!Identity) return {};
		Input.Identity = *Identity;
		return Input;
	}

	auto FPhysicsCookHelper::Cook(const FCookBodySetupInfo& Info, const FAssetBuildTaskContext& Control)
		-> std::expected<FPhysicsCookResult, FPhysicsCookFailure>
	{
		if (Control.IsCancelled()) return {};
		if (Info.Mode != EBodySetupCollisionSourceMode::None
			&& !PhysicsPrivate::CheckCookReservation(Info.TriangleMeshDesc.Positions.capacity(), Info.TriangleMeshDesc.Indices.capacity(), Control.MaximumWorkingSetBytes))
			return std::unexpected(FPhysicsCookFailure{"Physics cook input and working set exceed the reservation."});
		auto Input = Capture(Info, Control);
		if (Control.IsCancelled()) return {};
		if (!Input) return std::unexpected(std::move(Input.error()));
		return CookCaptured(*Input, Control);
	}

	auto FPhysicsCookHelper::CookCaptured(const FPhysicsCookInput& Input, const FAssetBuildTaskContext& Control)
		-> std::expected<FPhysicsCookResult, FPhysicsCookFailure>
	{
		const auto Cancel = [&] { return Control.IsCancelled(); };
		if (Cancel()) return {};
		if (Input.GetMode() == EBodySetupCollisionSourceMode::None) return FPhysicsCookResult{};
		if (auto Valid = PhysicsPrivate::CheckCookInput(Input.GetPositions().GetSize() / sizeof(FVector3f),
			Input.GetIndices().GetSize() / sizeof(uint32), Input.GetMode(), Input.GetPolicy(), Control.MaximumWorkingSetBytes); !Valid)
			return std::unexpected(std::move(Valid.error()));
		if (!PhysicsPrivate::CheckCookReservation(Input.GetPositions().GetRetainedCapacityBytes() / sizeof(FVector3f),
			Input.GetIndices().GetRetainedCapacityBytes() / sizeof(uint32), Control.MaximumWorkingSetBytes))
			return std::unexpected(FPhysicsCookFailure{"Physics cook input and working set exceed the reservation."});
#if DURIN_WITH_EDITOR
		auto Definition = MakePhysicsCookSessionDefinition(Input.GetMode(), Input.GetPolicy(), FCollisionCookedData::GetBuildVersion());
		if (!Definition) return std::unexpected(FPhysicsCookFailure{"Physics build definition is invalid."});
		DerivedData::FBuildRequestOptions Options;
		Options.Policy.StoreOnBuild = Input.ShouldPersist();
		Options.Policy.MaximumWorkingSetBytes = Control.MaximumWorkingSetBytes;
		Options.Policy.InputLimits.MaximumTotalBytes = MaximumPhysicsCollisionPayloadBytes;
		Options.Policy.OutputLimits.MaximumTotalBytes = MaximumPhysicsCollisionPayloadBytes;
		const uint64 MaximumBytes = std::min(MaximumPhysicsCollisionPayloadBytes, Control.MaximumWorkingSetBytes / 16);
		Options.Policy.PersistenceLimits.MaximumTotalBytes = MaximumBytes; Options.Policy.MaximumEncodedBytes = MaximumBytes;
		Options.Cancellation = Control.Cancellation.WithObserver([Control] { return Control.IsCancelled(); });
		auto Built = BuildPhysicsDefinition(std::move(*Definition),
			PhysicsPrivate::MakeCollisionInputResolver(Input), std::move(Options));
		if (!Built) return std::unexpected(FPhysicsCookFailure{
			Built.error().Description, EPhysicsCookStage::Cook});
		auto& Completion = *Built;
		if (Completion.GetStatus() == DerivedData::EStatus::Canceled) Control.Cancellation.Cancel();
		if (Cancel()) return {};
		if (Completion.GetStatus() == DerivedData::EStatus::Error)
		{
			std::string Description = "Physics derived-data build failed.";
			if (!Completion.GetDiagnostic().empty()) Description = Completion.GetDiagnostic();
			if (const auto* Output = Completion.GetOutput(); Output && !Output->GetMessages().empty()) Description = Output->GetMessages().back().Text;
			else if (const auto* Output = Completion.GetOutput(); Output && !Output->GetLogs().empty()) Description = Output->GetLogs().back().Text;
			return std::unexpected(FPhysicsCookFailure{std::move(Description), EPhysicsCookStage::Cook});
		}
		auto Result = PhysicsPrivate::AssembleSharedOutput(*Completion.GetOutput(), Input.GetMode(), Input.GetPolicy(), Cancel);
		if (Cancel()) return {};
		if (!Result) return std::unexpected(FPhysicsCookFailure{std::move(Result.error())});
		return std::move(*Result);
#else
		std::vector<FVector3> Positions;
		const auto Source = *Input.GetPositions().GetNativeView<FVector3f>();
		Positions.reserve(Source.size());
		for (size_t Index = 0; Index < Source.size(); ++Index)
		{
			if (Index % 256 == 0 && Cancel()) return {};
			Positions.emplace_back(Source[Index]);
		}
		FCollisionGeometryBuildDiagnostics Diagnostics;
		auto Geometry = Input.GetMode() == EBodySetupCollisionSourceMode::ConvexHullFromLOD0
			? FCollisionGeometryRef::BuildConvexHull(Positions, &Diagnostics, Cancel)
			: FCollisionGeometryRef::BuildTriangleMesh(Positions, *Input.GetIndices().GetNativeView<uint32>(), &Diagnostics, Cancel);
		if (Cancel()) return {};
		if (!Geometry) return std::unexpected(FPhysicsCookFailure{std::format("Physics geometry construction failed (status {}).", int(Diagnostics.Status))});
		FPhysicsCookResult Result;
		(Input.GetMode() == EBodySetupCollisionSourceMode::ConvexHullFromLOD0 ? Result.Simple : Result.Complex) = std::move(Geometry);
		return Result;
#endif
	}
}
