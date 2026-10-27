#include "Physics/PhysicsCookHelper.h"
#include "PhysicsCookInputPrivate.h"
#if DURIN_WITH_EDITOR
#include "DerivedDataBuildSession.h"
#include "Asset/ResourceBuildSessions.h"
#include "PhysicsCookDerivedDataKey.h"
#include "PhysicsBuildFunction.h"
#include "PhysicsSharedOutput.h"
#include "TargetPlatform.h"
#endif

namespace Durin
{
#if DURIN_WITH_EDITOR
	namespace
	{
		AssetPrivate::FResourceBuildSessions PhysicsSessions;

		auto GetPhysicsSession(DerivedData::EBuildCacheExecution Execution)
			-> std::expected<std::shared_ptr<DerivedData::FBuildSession>, DerivedData::FBuildAdmissionError>
		{
			static std::once_flag RegisterOnce;
			std::call_once(RegisterOnce, [] {
				if (!DerivedData::GetBuild().Register(PhysicsPrivate::MakeCollisionBuildFunction()))
					throw std::runtime_error("Failed to register the physics derived-data build function.");
			});
			return PhysicsSessions.Get(Execution);
		}
		auto MakePhysicsOptions(const FPhysicsCookInput& Input, const FAssetBuildTaskContext& Control) -> DerivedData::FBuildRequestOptions
		{
			DerivedData::FBuildRequestOptions Options;
			Options.Policy.StoreOnBuild = Input.ShouldPersist();
			Options.Policy.MaximumWorkingSetBytes = Control.MaximumWorkingSetBytes;
			Options.Policy.InputLimits.MaximumTotalBytes = MaximumPhysicsCollisionPayloadBytes;
			Options.Policy.OutputLimits.MaximumTotalBytes = MaximumPhysicsCollisionPayloadBytes;
			const uint64 MaximumBytes = std::min(MaximumPhysicsCollisionPayloadBytes, Control.MaximumWorkingSetBytes / 16);
			Options.Policy.PersistenceLimits.MaximumTotalBytes = MaximumBytes; Options.Policy.MaximumEncodedBytes = MaximumBytes;
			Options.Cancellation = Control.Cancellation;
			return Options;
		}

	}
	namespace PhysicsPrivate
	{
		auto SetAsyncCacheForTests(std::shared_ptr<DerivedData::ICache> Cache) -> void
		{
			std::shared_ptr<DerivedData::IBuild> Service;
			if (Cache)
			{
				Service = DerivedData::CreateBuild({.Cache = std::move(Cache)});
				require(Service->Register(MakeCollisionBuildFunction()).has_value());
			}
			require(PhysicsSessions.SetTaskService(std::move(Service)) == DerivedData::EBuildDrainResult::Drained);
		}
		auto DispatchCookBuild(const FPhysicsCookInput& Input, DerivedData::IRequestOwner& Owner,
			const FAssetBuildTaskContext& Control, DerivedData::FBuildCompletionCallback Completion) -> std::expected<void, FPhysicsCookError>
		{
			if (Control.IsCancelled()) return PhysicsPrivate::CookCancelled();
			auto Definition = MakePhysicsCookSessionDefinition(Input.GetMode(), FCollisionCookedData::GetBuildVersion(), 0, Input.GetTargetPlatform());
			if (!Definition) return PhysicsPrivate::CookFailure(MakeAssetBuildDiagnostic("Physics build definition is invalid.", "Cook"));
			auto Session = GetPhysicsSession(DerivedData::EBuildCacheExecution::Task);
			if (!Session) return PhysicsPrivate::CookFailure(MakeAssetBuildDiagnostic(Session.error().Description, "Cook"));
			auto Options = MakePhysicsOptions(Input, Control);
			Options.InputResolver = MakeCollisionInputResolver(Input);
			auto Admitted = (*Session)->Build(std::move(*Definition), Owner, std::move(Completion), {}, std::move(Options));
			if (!Admitted) return PhysicsPrivate::CookFailure(MakeAssetBuildDiagnostic(Admitted.error().Description, "Cook"));
			return {};
		}
		auto ConsumeCookBuild(EBodySetupCollisionSourceMode Mode,
			DerivedData::FBuildCompleteParams Completion, const FAssetBuildTaskContext& Control) -> std::expected<FPhysicsCookResult, FPhysicsCookError>
		{
			const auto Cancel = [&] { return Control.IsCancelled(); };
			if (Completion.GetStatus() == DerivedData::EStatus::Canceled) Control.Cancellation.Cancel();
			if (Cancel()) return PhysicsPrivate::CookCancelled();
			if (Completion.GetStatus() == DerivedData::EStatus::Error)
			{
				std::string Description = "Physics derived-data build failed.";
				if (!Completion.GetDiagnostic().empty()) Description = Completion.GetDiagnostic();
				if (const auto* Output = Completion.GetOutput(); Output && !Output->GetMessages().empty()) Description = Output->GetMessages().back().Text;
				else if (const auto* Output = Completion.GetOutput(); Output && !Output->GetLogs().empty()) Description = Output->GetLogs().back().Text;
				return PhysicsPrivate::CookFailure(MakeAssetBuildDiagnostic(std::move(Description), "Cook"));
			}
			auto Result = PhysicsPrivate::AssembleSharedOutput(*Completion.GetOutput(), Mode, Cancel);
			if (Cancel()) return PhysicsPrivate::CookCancelled();
			if (!Result) return PhysicsPrivate::CookFailure(MakeAssetBuildDiagnostic(std::move(Result.error()), "Cook"));
			return std::move(*Result);
		}
		auto ShutdownBuildSessions() -> void
		{ require(PhysicsSessions.Close() == DerivedData::EBuildDrainResult::Drained); }
	}
#endif

	auto FPhysicsCookHelper::Capture(FCookBodySetupInfo Info, const FAssetBuildTaskContext& Control, EAssetPayloadTargetPlatform TargetPlatform)
		-> std::expected<FPhysicsCookInput, FPhysicsCookError>
	{
		if (Control.IsCancelled()) return PhysicsPrivate::CookCancelled();
		FPhysicsCookInput Input;
		Input.TargetPlatform = TargetPlatform;
		Input.Mode = Info.Mode; Input.bPersist = Info.bPersistDerivedData;
		if (Info.Mode == EBodySetupCollisionSourceMode::None) return Input;
#if DURIN_WITH_EDITOR
		const auto Target = ResolveTargetPlatform(static_cast<ECookTargetPlatform>(TargetPlatform));
		if (!Target || !Target->SupportsCollisionPayload())
			return PhysicsPrivate::CookFailure(MakeAssetBuildDiagnostic("Physics collision representation is unsupported for the requested target.", "Cook"));
#else
		if (TargetPlatform != EAssetPayloadTargetPlatform::Win64)
			return PhysicsPrivate::CookFailure(MakeAssetBuildDiagnostic("Physics collision representation is unsupported for the requested target.", "Cook"));
#endif
		if (auto Valid = PhysicsPrivate::CheckCookInput(Info.TriangleMeshDesc.Positions.size(), Info.TriangleMeshDesc.Indices.size(),
			Info.Mode, Control.MaximumWorkingSetBytes); !Valid) return PhysicsPrivate::CookFailure(std::move(Valid.error()));
		if (!PhysicsPrivate::CheckCookReservation(Info.TriangleMeshDesc.Positions.capacity(), Info.TriangleMeshDesc.Indices.capacity(), Control.MaximumWorkingSetBytes))
			return PhysicsPrivate::CookFailure(MakeAssetBuildDiagnostic("Physics cook input and working set exceed the reservation.", "Cook"));
		Input.Positions = FSharedByteBuffer::TakeNative(std::move(Info.TriangleMeshDesc.Positions));
		Input.Indices = FSharedByteBuffer::TakeNative(std::move(Info.TriangleMeshDesc.Indices));
		auto Identity = PhysicsPrivate::HashInput(Input.Positions.GetBytes(), Input.Indices.GetBytes(), [&] { return Control.IsCancelled(); });
		if (!Identity) return PhysicsPrivate::CookCancelled();
		Input.Identity = *Identity;
		return Input;
	}

	auto FPhysicsCookHelper::Cook(const FCookBodySetupInfo& Info, const FAssetBuildTaskContext& Control)
		-> std::expected<FPhysicsCookResult, FPhysicsCookError>
	{
		if (Control.IsCancelled()) return PhysicsPrivate::CookCancelled();
		if (Info.Mode != EBodySetupCollisionSourceMode::None
			&& !PhysicsPrivate::CheckCookReservation(Info.TriangleMeshDesc.Positions.capacity(), Info.TriangleMeshDesc.Indices.capacity(), Control.MaximumWorkingSetBytes))
			return PhysicsPrivate::CookFailure(MakeAssetBuildDiagnostic("Physics cook input and working set exceed the reservation.", "Cook"));
		auto Input = Capture(Info, Control);
		if (!Input) return std::unexpected(std::move(Input.error()));
		return CookCaptured(*Input, Control);
	}

	auto FPhysicsCookHelper::CookCaptured(const FPhysicsCookInput& Input, const FAssetBuildTaskContext& Control)
		-> std::expected<FPhysicsCookResult, FPhysicsCookError>
	{
		const auto Cancel = [&] { return Control.IsCancelled(); };
		if (Cancel()) return PhysicsPrivate::CookCancelled();
		if (Input.GetMode() == EBodySetupCollisionSourceMode::None) return FPhysicsCookResult{};
		if (auto Valid = PhysicsPrivate::CheckCookInput(Input.GetPositions().GetSize() / sizeof(FVector3f),
			Input.GetIndices().GetSize() / sizeof(uint32), Input.GetMode(), Control.MaximumWorkingSetBytes); !Valid)
			return PhysicsPrivate::CookFailure(std::move(Valid.error()));
		if (!PhysicsPrivate::CheckCookReservation(Input.GetPositions().GetRetainedCapacityBytes() / sizeof(FVector3f),
			Input.GetIndices().GetRetainedCapacityBytes() / sizeof(uint32), Control.MaximumWorkingSetBytes))
			return PhysicsPrivate::CookFailure(MakeAssetBuildDiagnostic("Physics cook input and working set exceed the reservation.", "Cook"));
#if DURIN_WITH_EDITOR
		auto Definition = MakePhysicsCookSessionDefinition(Input.GetMode(), FCollisionCookedData::GetBuildVersion(), 0, Input.GetTargetPlatform());
		if (!Definition) return PhysicsPrivate::CookFailure(MakeAssetBuildDiagnostic("Physics build definition is invalid.", "Cook"));
		auto Session = GetPhysicsSession(DerivedData::EBuildCacheExecution::Inline);
		if (!Session) return PhysicsPrivate::CookFailure(MakeAssetBuildDiagnostic(Session.error().Description, "Cook"));
		auto Options = MakePhysicsOptions(Input, Control);
		Options.InputResolver = PhysicsPrivate::MakeCollisionInputResolver(Input);
		DerivedData::FRequestOwner Owner;
		std::optional<DerivedData::FBuildCompleteParams> Completion;
		auto Admitted = (*Session)->Build(std::move(*Definition), Owner, [&](auto Value) { Completion.emplace(std::move(Value)); }, {}, std::move(Options));
		if (!Admitted) return PhysicsPrivate::CookFailure(MakeAssetBuildDiagnostic(Admitted.error().Description, "Cook"));
		require(Owner.Wait() == DerivedData::ERequestWaitResult::Completed);
		require(Completion.has_value());
		return PhysicsPrivate::ConsumeCookBuild(Input.GetMode(), std::move(*Completion), Control);
#else
		std::vector<FVector3> Positions;
		const auto Source = *Input.GetPositions().GetNativeView<FVector3f>();
		Positions.reserve(Source.size());
		for (size_t Index = 0; Index < Source.size(); ++Index)
		{
			if (Index % 256 == 0 && Cancel()) return PhysicsPrivate::CookCancelled();
			Positions.emplace_back(Source[Index]);
		}
		FCollisionGeometryBuildDiagnostics Diagnostics;
		auto Geometry = Input.GetMode() == EBodySetupCollisionSourceMode::ConvexHullFromLOD0
			? FCollisionGeometryRef::BuildConvexHull(Positions, &Diagnostics, Cancel)
			: FCollisionGeometryRef::BuildTriangleMesh(Positions, *Input.GetIndices().GetNativeView<uint32>(), &Diagnostics, Cancel);
		if (Cancel()) return PhysicsPrivate::CookCancelled();
		if (!Geometry) return PhysicsPrivate::CookFailure(MakeAssetBuildDiagnostic(std::format("Physics geometry construction failed (status {}).", int(Diagnostics.Status)), "Cook"));
		FPhysicsCookResult Result;
		(Input.GetMode() == EBodySetupCollisionSourceMode::ConvexHullFromLOD0 ? Result.Simple : Result.Complex) = std::move(Geometry);
		return Result;
#endif
	}
}
