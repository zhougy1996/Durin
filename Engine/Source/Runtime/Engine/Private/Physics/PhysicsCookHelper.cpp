#include "Physics/PhysicsCookHelper.h"
#include "PhysicsCookInputPrivate.h"
#if DURIN_WITH_EDITOR
	#include "BuildServiceBackendPrivate.h"
	#include "CacheDispatchPrivate.h"
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
		auto MakePhysicsOptions(const FPhysicsCookInput& Input, uint64 MaximumWorkingSetBytes) -> DerivedData::FBuildRequestOptions
		{
			DerivedData::FBuildRequestOptions Options;
			Options.Policy.StoreOnBuild = Input.ShouldPersist();
			Options.Policy.MaximumWorkingSetBytes = MaximumWorkingSetBytes;
			Options.Policy.InputLimits.MaximumTotalBytes = MaximumPhysicsCollisionPayloadBytes;
			Options.Policy.OutputLimits.MaximumTotalBytes = MaximumPhysicsCollisionPayloadBytes;
			const uint64 MaximumBytes = std::min(MaximumPhysicsCollisionPayloadBytes, MaximumWorkingSetBytes / 16);
			Options.Policy.PersistenceLimits.MaximumTotalBytes = MaximumBytes; Options.Policy.MaximumEncodedBytes = MaximumBytes;
			return Options;
		}

	}
	namespace PhysicsPrivate
	{
		auto SetAsyncCacheForTests(std::shared_ptr<DerivedData::ICacheBackend> Cache) -> void
		{
			std::shared_ptr<DerivedData::IBuild> Service;
			if (Cache)
			{
				Service = DerivedData::Private::CreateBuildForBackend({.Cache = std::move(Cache)});
				require(Service->Register(MakeCollisionBuildFunction()).has_value());
			}
			require(PhysicsSessions.SetTaskService(std::move(Service)) == DerivedData::EBuildDrainResult::Drained);
		}
		auto DispatchCookBuild(const FPhysicsCookInput& Input, DerivedData::IRequestOwner& Owner,
			uint64 MaximumWorkingSetBytes, DerivedData::FBuildCompletionCallback Completion) -> std::expected<void, std::string>
		{
			auto Definition = MakePhysicsCookSessionDefinition(Input.GetMode(), FCollisionCookedData::GetBuildVersion(), 0, Input.GetTargetPlatform());
			if (!Definition) return std::unexpected(MakeAssetBuildDiagnostic("Physics build definition is invalid.", "Cook"));
			auto Session = GetPhysicsSession(DerivedData::EBuildCacheExecution::Task);
			if (!Session) return std::unexpected(MakeAssetBuildDiagnostic(Session.error().Description, "Cook"));
			auto Options = MakePhysicsOptions(Input, MaximumWorkingSetBytes);
			Options.InputResolver = MakeCollisionInputResolver(Input);
			(*Session)->Build(std::move(*Definition), Owner, std::move(Completion), {}, std::move(Options));

			return {};
		}
		auto ConsumeCookBuild(EBodySetupCollisionSourceMode Mode,
			DerivedData::FBuildCompleteParams Completion) -> std::expected<FPhysicsCookResult, std::string>
		{
			if (Completion.GetStatus() != DerivedData::EStatus::Ok)
			{
				std::string Description = "Physics derived-data build failed.";
				if (!Completion.GetDiagnostic().empty()) Description = Completion.GetDiagnostic();
				if (const auto* Output = Completion.GetOutput(); Output && !Output->GetMessages().empty()) Description = Output->GetMessages().back().Text;
				else if (const auto* Output = Completion.GetOutput(); Output && !Output->GetLogs().empty()) Description = Output->GetLogs().back().Text;
				return std::unexpected(MakeAssetBuildDiagnostic(std::move(Description), "Cook"));
			}
			auto Result = PhysicsPrivate::AssembleSharedOutput(*Completion.GetOutput(), Mode);

			if (!Result) return std::unexpected(MakeAssetBuildDiagnostic(std::move(Result.error()), "Cook"));
			return std::move(*Result);
		}
		auto ShutdownBuildSessions() -> void
		{ require(PhysicsSessions.Close() == DerivedData::EBuildDrainResult::Drained); }
	}
#endif

	auto FPhysicsCookHelper::Capture(FCookBodySetupInfo Info, uint64 MaximumWorkingSetBytes, EAssetPayloadTargetPlatform TargetPlatform)
		-> std::expected<FPhysicsCookInput, std::string>
	{
		FPhysicsCookInput Input;
		Input.TargetPlatform = TargetPlatform;
		Input.Mode = Info.Mode; Input.bPersist = Info.bPersistDerivedData;
		if (Info.Mode == EBodySetupCollisionSourceMode::None) return Input;
#if DURIN_WITH_EDITOR
		const auto Target = ResolveTargetPlatform(static_cast<ECookTargetPlatform>(TargetPlatform));
		if (!Target || !Target->SupportsCollisionPayload())
			return std::unexpected(MakeAssetBuildDiagnostic("Physics collision representation is unsupported for the requested target.", "Cook"));
#else
		if (TargetPlatform != EAssetPayloadTargetPlatform::Win64)
			return std::unexpected(MakeAssetBuildDiagnostic("Physics collision representation is unsupported for the requested target.", "Cook"));
#endif
		if (auto Valid = PhysicsPrivate::CheckCookInput(Info.TriangleMeshDesc.Positions.size(), Info.TriangleMeshDesc.Indices.size(),
			Info.Mode, MaximumWorkingSetBytes); !Valid) return std::unexpected(std::move(Valid.error()));
		if (!PhysicsPrivate::CheckCookReservation(Info.TriangleMeshDesc.Positions.capacity(), Info.TriangleMeshDesc.Indices.capacity(), MaximumWorkingSetBytes))
			return std::unexpected(MakeAssetBuildDiagnostic("Physics cook input and working set exceed the reservation.", "Cook"));
		Input.Positions = FSharedByteBuffer::TakeNative(std::move(Info.TriangleMeshDesc.Positions));
		Input.Indices = FSharedByteBuffer::TakeNative(std::move(Info.TriangleMeshDesc.Indices));
		Input.Identity = PhysicsPrivate::HashInput(Input.Positions.GetBytes(), Input.Indices.GetBytes());
		return Input;
	}

	auto FPhysicsCookHelper::Cook(const FCookBodySetupInfo& Info, uint64 MaximumWorkingSetBytes)
		-> std::expected<FPhysicsCookResult, std::string>
	{
		if (Info.Mode != EBodySetupCollisionSourceMode::None
			&& !PhysicsPrivate::CheckCookReservation(Info.TriangleMeshDesc.Positions.capacity(), Info.TriangleMeshDesc.Indices.capacity(), MaximumWorkingSetBytes))
			return std::unexpected(MakeAssetBuildDiagnostic("Physics cook input and working set exceed the reservation.", "Cook"));
		auto Input = Capture(Info, MaximumWorkingSetBytes);
		if (!Input) return std::unexpected(std::move(Input.error()));
		return CookCaptured(*Input, MaximumWorkingSetBytes);
	}

	auto FPhysicsCookHelper::CookCaptured(const FPhysicsCookInput& Input, uint64 MaximumWorkingSetBytes)
		-> std::expected<FPhysicsCookResult, std::string>
	{
		if (Input.GetMode() == EBodySetupCollisionSourceMode::None) return FPhysicsCookResult{};
		if (auto Valid = PhysicsPrivate::CheckCookInput(Input.GetPositions().GetSize() / sizeof(FVector3f),
			Input.GetIndices().GetSize() / sizeof(uint32), Input.GetMode(), MaximumWorkingSetBytes); !Valid)
			return std::unexpected(std::move(Valid.error()));
		if (!PhysicsPrivate::CheckCookReservation(Input.GetPositions().GetRetainedCapacityBytes() / sizeof(FVector3f),
			Input.GetIndices().GetRetainedCapacityBytes() / sizeof(uint32), MaximumWorkingSetBytes))
			return std::unexpected(MakeAssetBuildDiagnostic("Physics cook input and working set exceed the reservation.", "Cook"));
#if DURIN_WITH_EDITOR
		auto Definition = MakePhysicsCookSessionDefinition(Input.GetMode(), FCollisionCookedData::GetBuildVersion(), 0, Input.GetTargetPlatform());
		if (!Definition) return std::unexpected(MakeAssetBuildDiagnostic("Physics build definition is invalid.", "Cook"));
		auto Session = GetPhysicsSession(DerivedData::EBuildCacheExecution::Inline);
		if (!Session) return std::unexpected(MakeAssetBuildDiagnostic(Session.error().Description, "Cook"));
		auto Options = MakePhysicsOptions(Input, MaximumWorkingSetBytes);
		Options.InputResolver = PhysicsPrivate::MakeCollisionInputResolver(Input);
		DerivedData::FRequestOwner Owner;
		std::optional<DerivedData::FBuildCompleteParams> Completion;
		(*Session)->Build(std::move(*Definition), Owner, [&](auto Value) { Completion.emplace(std::move(Value)); }, {}, std::move(Options));

		require(Owner.Wait() == DerivedData::ERequestWaitResult::Completed);
		require(Completion.has_value());
		return PhysicsPrivate::ConsumeCookBuild(Input.GetMode(), std::move(*Completion));
#else
		std::vector<FVector3> Positions;
		const auto Source = *Input.GetPositions().GetNativeView<FVector3f>();
		Positions.reserve(Source.size());
		for (size_t Index = 0; Index < Source.size(); ++Index)
		{
			Positions.emplace_back(Source[Index]);
		}
		FCollisionGeometryBuildDiagnostics Diagnostics;
		auto Geometry = Input.GetMode() == EBodySetupCollisionSourceMode::ConvexHullFromLOD0
			? FCollisionGeometryRef::BuildConvexHull(Positions, &Diagnostics)
			: FCollisionGeometryRef::BuildTriangleMesh(Positions, *Input.GetIndices().GetNativeView<uint32>(), &Diagnostics);

		if (!Geometry) return std::unexpected(MakeAssetBuildDiagnostic(std::format("Physics geometry construction failed (status {}).", int(Diagnostics.Status)), "Cook"));
		FPhysicsCookResult Result;
		(Input.GetMode() == EBodySetupCollisionSourceMode::ConvexHullFromLOD0 ? Result.Simple : Result.Complex) = std::move(Geometry);
		return Result;
#endif
	}
}
