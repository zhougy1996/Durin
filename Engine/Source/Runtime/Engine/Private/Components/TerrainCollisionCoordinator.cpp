#include "Components/TerrainCollisionCoordinator.h"

#include "Engine/World.h"
#include "Terrain/TerrainHeightmap.h"
#include "Terrain/TerrainHeightmapPostLoad.h"

namespace Durin
{
	namespace
	{
		constexpr uint32 MaximumTerrainCollisionSamples = 1025;
	}

	FTerrainCollisionCoordinator::FTerrainCollisionCoordinator(DTerrainComponent& InOwner)
		: Owner(InOwner)
	{
	}

	FTerrainCollisionCoordinator::~FTerrainCollisionCoordinator()
	{
		CancelBuild();
	}

	auto FTerrainCollisionCoordinator::SetStatus(
		ETerrainCollisionStatus Status, std::string Diagnostic) const -> void
	{
		Owner.CollisionStatus = Status;
		Owner.LastCollisionDiagnostic = std::move(Diagnostic);
	}

	auto FTerrainCollisionCoordinator::ResetCache() const -> void
	{
		CachedGeometry = {};
		CachedPayload.reset();
		CachedHeightmapRevision = 0;
		CachedDiagnostics = {};
	}

	auto FTerrainCollisionCoordinator::OnRegistered() -> void
	{
		if (Owner.IsRegistered()
			&& Owner.GetPhysicsStateCreationPolicy() == EPhysicsStateCreationPolicy::OnDemand
			&& Owner.GetCollisionEnabled() != ECollisionEnabled::NoCollision)
			SetStatus(ETerrainCollisionStatus::Dormant);
	}

	auto FTerrainCollisionCoordinator::OnUnregistered() -> void
	{
		CancelBuild();
		SetStatus(ETerrainCollisionStatus::Unavailable);
	}

	auto FTerrainCollisionCoordinator::CancelBuild() -> void
	{
		std::shared_ptr<FBuildState> Build = std::move(ActiveBuild);
		if (!Build) return;
		Build->Cancellation.RequestCancellation();
		(void)CancelTask(Build->Worker);
		(void)CancelTask(Build->Publisher);
	}

	auto FTerrainCollisionCoordinator::OnCollisionSettingsChanged() -> void
	{
		CancelBuild();
		Owner.DestroyPhysicsState();
		SetStatus(Owner.IsRegistered() && Owner.GetCollisionEnabled() != ECollisionEnabled::NoCollision
			? ETerrainCollisionStatus::Dormant : ETerrainCollisionStatus::Unavailable);
		if (Owner.IsRegistered() && Owner.GetCollisionEnabled() != ECollisionEnabled::NoCollision
			&& Owner.GetPhysicsStateCreationPolicy() == EPhysicsStateCreationPolicy::DeferredRequired)
			(void)RequestPhysicsStateCreation(false);
	}

	auto FTerrainCollisionCoordinator::OnSourceChanged() -> void
	{
		++CollisionRevision;
		CancelBuild();
		Owner.DestroyPhysicsState();
		ResetCache();
		SetStatus(Owner.IsRegistered() && Owner.GetCollisionEnabled() != ECollisionEnabled::NoCollision
			? ETerrainCollisionStatus::Dormant : ETerrainCollisionStatus::Unavailable);
		if (Owner.IsRegistered() && Owner.GetCollisionEnabled() != ECollisionEnabled::NoCollision
			&& Owner.GetPhysicsStateCreationPolicy() == EPhysicsStateCreationPolicy::DeferredRequired)
			(void)RequestPhysicsStateCreation(false);
	}

	auto FTerrainCollisionCoordinator::PrepareForSourceRevisionChange() -> void
	{
		CancelBuild();
		Owner.DestroyPhysicsState();
		ResetCache();
		SetStatus(Owner.GetCollisionEnabled() == ECollisionEnabled::NoCollision
			? ETerrainCollisionStatus::Unavailable : ETerrainCollisionStatus::Dormant);
	}

	auto FTerrainCollisionCoordinator::GetStatus() const -> ETerrainCollisionStatus
	{
		if (Owner.GetCollisionEnabled() == ECollisionEnabled::NoCollision)
			return ETerrainCollisionStatus::Unavailable;
		if (Owner.IsRegistered()
			&& Owner.GetPhysicsStateCreationPolicy() == EPhysicsStateCreationPolicy::OnDemand
			&& !Owner.GetPhysicsActorHandle().IsValid()
			&& Owner.CollisionStatus != ETerrainCollisionStatus::Building
			&& Owner.CollisionStatus != ETerrainCollisionStatus::BuildFailed)
			return ETerrainCollisionStatus::Dormant;
		return Owner.CollisionStatus;
	}

	auto FTerrainCollisionCoordinator::GetDiagnostic() const -> const std::string&
	{
		return Owner.LastCollisionDiagnostic;
	}

	auto FTerrainCollisionCoordinator::CacheMatches(
		const std::shared_ptr<const FTerrainHeightmapPayload>& Payload,
		uint64 AssetRevision) const -> bool
	{
		return CachedGeometry.IsValid() && CachedPayload == Payload
			&& CachedHeightmapRevision == AssetRevision
			&& CachedSpacingX == Owner.SpacingX && CachedSpacingY == Owner.SpacingY
			&& CachedHeightScale == Owner.HeightScale && CachedHeightOffset == Owner.HeightOffset;
	}

	auto FTerrainCollisionCoordinator::RequestPhysicsStateCreation(bool bWaitUntilReady) -> bool
	{
		if (Owner.GetCollisionEnabled() == ECollisionEnabled::NoCollision)
		{
			CancelBuild();
			Owner.DestroyPhysicsState();
			SetStatus(ETerrainCollisionStatus::Unavailable);
			return true;
		}
		if (!Owner.IsRegistered() || !Owner.GetPhysicsWorld())
			return SetFailure(ETerrainCollisionStatus::BuildFailed,
				"Terrain collision cannot be requested before component registration.");
		if (Owner.GetPhysicsActorHandle().IsValid()
			&& Owner.GetPublishedBodySetupRevision() == CollisionRevision
			&& Owner.CollisionStatus == ETerrainCollisionStatus::Ready) return true;

		if (ActiveBuild)
		{
			if (!bWaitUntilReady) return true;
			const std::shared_ptr<FBuildState> Build = ActiveBuild;
			const ETaskState State = WaitTask(Build->Worker);
			if (State == ETaskState::Succeeded) return PublishCompletedBuild(Build);
			CancelBuild();
			return SetFailure(ETerrainCollisionStatus::BuildFailed,
				"Terrain collision worker did not complete successfully.");
		}

		std::string Error;
		if (!Owner.ValidateProperties(Error))
			return SetFailure(ETerrainCollisionStatus::InvalidProperties, std::move(Error));
		if (!Owner.Heightmap)
			return SetFailure(ETerrainCollisionStatus::MissingHeightmap,
				"Terrain collision requires an assigned heightmap.");
		if (Owner.Heightmap->GetStatus() == ETerrainHeightmapStatus::Loading
			|| Owner.Heightmap->GetStatus() == ETerrainHeightmapStatus::Rebuilding)
		{
			if (!bWaitUntilReady)
			{
				SetStatus(ETerrainCollisionStatus::Building,
					"Terrain collision is waiting for the asynchronous heightmap payload.");
				return true;
			}
			if (!WaitForTerrainHeightmapAuthoringLoad(*Owner.Heightmap, Error))
				return SetFailure(ETerrainCollisionStatus::BuildFailed, std::move(Error));
			return RequestPhysicsStateCreation(true);
		}
		const std::shared_ptr<const FTerrainHeightmapPayload> Payload = Owner.Heightmap->GetPayload();
		if (!Payload || !Payload->HasValidLayout() || Payload->Width < 2 || Payload->Height < 2)
			return SetFailure(ETerrainCollisionStatus::InvalidPayload,
				"Terrain collision requires a valid heightmap with at least two samples on each axis.");
		if (Payload->Width > MaximumTerrainCollisionSamples || Payload->Height > MaximumTerrainCollisionSamples)
			return SetFailure(ETerrainCollisionStatus::ExtentRejected, std::format(
				"Terrain heightmap {}x{} exceeds the T2 collision ceiling of {}x{} samples.",
				Payload->Width, Payload->Height, MaximumTerrainCollisionSamples, MaximumTerrainCollisionSamples));

		const uint64 AssetRevision = Owner.Heightmap->GetRevision();
		if (CacheMatches(Payload, AssetRevision))
		{
			Owner.DPrimitiveComponent::RecreatePhysicsState();
			return Owner.GetPhysicsActorHandle().IsValid();
		}
		if (!IsTaskSchedulerRunning())
		{
			if (bWaitUntilReady)
				return Owner.DPrimitiveComponent::RequestPhysicsStateCreation(true);
			return SetFailure(ETerrainCollisionStatus::BuildFailed,
				"The CPU task scheduler is not running for the required Terrain collision build.");
		}

		auto Build = std::make_shared<FBuildState>();
		Build->ComponentHandle = MakeObjectHandle(&Owner);
		Build->WorldHandle = MakeObjectHandle(Owner.GetPhysicsWorld());
		Build->RegistrationGeneration = Owner.GetPhysicsRegistrationGeneration();
		Build->AssetRevision = AssetRevision;
		Build->CollisionRevision = CollisionRevision;
		Build->Payload = Payload;
		Build->SpacingX = Owner.SpacingX;
		Build->SpacingY = Owner.SpacingY;
		Build->HeightScale = Owner.HeightScale;
		Build->HeightOffset = Owner.HeightOffset;

		static const FTaskAttribution BuildAttribution =
			RegisterTaskAttribution("TerrainCollision", "BuildHeightField");
		FTaskLaunchOptions BuildOptions;
		BuildOptions.CancellationToken = Build->Cancellation.GetToken();
		BuildOptions.Attribution = BuildAttribution;
		Build->Worker = LaunchCancelableTask("TerrainCollision.BuildHeightField",
			[Build](const FTaskCancellationToken& Token) {
				if (Token.IsCancellationRequested())
				{
					std::lock_guard Lock(Build->ResultMutex);
					Build->bCanceled = true;
					return;
				}
				FCollisionGeometryBuildDiagnostics Diagnostics;
				FCollisionGeometryRef Geometry = FCollisionGeometryRef::BuildHeightField(
					Build->Payload->Width, Build->Payload->Height, Build->Payload->Samples,
					Build->SpacingX, Build->SpacingY, Build->HeightScale, Build->HeightOffset,
					&Diagnostics);
				std::lock_guard Lock(Build->ResultMutex);
				Build->bCanceled = Token.IsCancellationRequested();
				Build->Geometry = Build->bCanceled ? FCollisionGeometryRef{} : std::move(Geometry);
				Build->Diagnostics = Diagnostics;
				Build->bWorkerCompleted = true;
			}, BuildOptions);
		if (!Build->Worker.IsValid())
		{
			if (bWaitUntilReady)
				return Owner.DPrimitiveComponent::RequestPhysicsStateCreation(true);
			return SetFailure(ETerrainCollisionStatus::BuildFailed,
				"The CPU task scheduler rejected the Terrain collision build.");
		}

		ActiveBuild = Build;
		SetStatus(ETerrainCollisionStatus::Building);
		FTaskContinuationOptions PublishOptions;
		PublishOptions.Target = ETaskTarget::GameThreadDeferred;
		PublishOptions.EstimatedPayloadBytes = sizeof(FObjectHandle) + sizeof(std::weak_ptr<FBuildState>);
		PublishOptions.Attribution = BuildAttribution;
		const std::weak_ptr<FBuildState> WeakBuild = Build;
		Build->Publisher = ThenOutcome(Build->Worker, "TerrainCollision.PublishHeightField",
			[WeakBuild](FTaskOutcome<void>) {
				const std::shared_ptr<FBuildState> Completed = WeakBuild.lock();
				if (!Completed) return;
				auto* Component = Cast<DTerrainComponent>(ResolveObjectHandle(Completed->ComponentHandle));
				if (IsValid(Component) && Component->CollisionCoordinator)
					(void)Component->CollisionCoordinator->PublishCompletedBuild(Completed);
			}, PublishOptions);
		if (!Build->Publisher.IsValid() && !bWaitUntilReady)
		{
			CancelBuild();
			return SetFailure(ETerrainCollisionStatus::BuildFailed,
				"The GameThread executor rejected Terrain collision publication.");
		}
		if (!bWaitUntilReady) return true;
		const ETaskState State = WaitTask(Build->Worker);
		if (State == ETaskState::Succeeded) return PublishCompletedBuild(Build);
		CancelBuild();
		return SetFailure(ETerrainCollisionStatus::BuildFailed,
			"Terrain collision worker did not complete successfully.");
	}

	auto FTerrainCollisionCoordinator::PublishCompletedBuild(
		const std::shared_ptr<FBuildState>& Build) -> bool
	{
		if (!Build || ActiveBuild != Build) return false;
		if (!Owner.IsRegistered()
			|| Owner.GetPhysicsRegistrationGeneration() != Build->RegistrationGeneration
			|| ResolveObjectHandle(Build->WorldHandle) != Owner.GetPhysicsWorld()
			|| !Owner.Heightmap || Owner.Heightmap->GetPayload() != Build->Payload
			|| Owner.Heightmap->GetRevision() != Build->AssetRevision
			|| CollisionRevision != Build->CollisionRevision
			|| Owner.SpacingX != Build->SpacingX || Owner.SpacingY != Build->SpacingY
			|| Owner.HeightScale != Build->HeightScale || Owner.HeightOffset != Build->HeightOffset
			|| Owner.GetCollisionEnabled() == ECollisionEnabled::NoCollision)
		{
			ActiveBuild.reset();
			SetStatus(ETerrainCollisionStatus::Dormant);
			return false;
		}

		FCollisionGeometryRef Geometry;
		FCollisionGeometryBuildDiagnostics Diagnostics;
		bool bCompleted = false;
		bool bCanceled = false;
		{
			std::lock_guard Lock(Build->ResultMutex);
			Geometry = Build->Geometry;
			Diagnostics = Build->Diagnostics;
			bCompleted = Build->bWorkerCompleted;
			bCanceled = Build->bCanceled;
		}
		ActiveBuild.reset();
		if (!bCompleted || bCanceled || !Geometry.IsValid())
			return SetFailure(ETerrainCollisionStatus::BuildFailed,
				bCanceled ? "Terrain collision build was canceled."
					: std::format("Terrain collision build failed with status {}.",
						static_cast<uint32>(Diagnostics.Status)));

		CachedGeometry = std::move(Geometry);
		CachedDiagnostics = Diagnostics;
		CachedPayload = Build->Payload;
		CachedHeightmapRevision = Build->AssetRevision;
		CachedSpacingX = Build->SpacingX;
		CachedSpacingY = Build->SpacingY;
		CachedHeightScale = Build->HeightScale;
		CachedHeightOffset = Build->HeightOffset;
		const auto InsertionStart = std::chrono::steady_clock::now();
		Owner.DPrimitiveComponent::RecreatePhysicsState();
		LastPhysicsInsertionNanoseconds = static_cast<uint64>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - InsertionStart).count());
		if (!Owner.GetPhysicsActorHandle().IsValid())
			return SetFailure(ETerrainCollisionStatus::BuildFailed,
				"Terrain collision geometry was built but physics-scene insertion failed.");
		SetStatus(ETerrainCollisionStatus::Ready);
		return true;
	}

	auto FTerrainCollisionCoordinator::SetFailure(
		ETerrainCollisionStatus Status, std::string Diagnostic) const -> bool
	{
		SetStatus(Status, std::move(Diagnostic));
		ResetCache();
		return false;
	}

	auto FTerrainCollisionCoordinator::GetFacts() const -> FTerrainCollisionFacts
	{
		FTerrainCollisionFacts Facts;
		Facts.Status = GetStatus();
		Facts.AssetRevision = Owner.Heightmap ? Owner.Heightmap->GetRevision() : 0;
		Facts.CollisionRevision = CollisionRevision;
		Facts.ResourceIdentity = CachedGeometry.GetIdentity();
		Facts.RetainedBytes = CachedGeometry.GetRetainedBytes();
		Facts.EstimatedPeakBytes = CachedDiagnostics.EstimatedPeakBytes;
		Facts.HashNanoseconds = CachedDiagnostics.HashNanoseconds;
		Facts.MatchNanoseconds = CachedDiagnostics.MatchNanoseconds;
		Facts.SampleCopyNanoseconds = CachedDiagnostics.SampleCopyNanoseconds;
		Facts.TreeBuildNanoseconds = CachedDiagnostics.TreeBuildNanoseconds;
		Facts.PhysicsInsertionNanoseconds = LastPhysicsInsertionNanoseconds;
		Facts.Width = CachedGeometry.GetHeightFieldWidth();
		Facts.Height = CachedGeometry.GetHeightFieldHeight();
		Facts.Cells = Facts.Width > 0 && Facts.Height > 0
			? (Facts.Width - 1) * (Facts.Height - 1) : 0;
		Facts.Nodes = CachedGeometry.GetNodeCount();
		Facts.MaximumDepth = CachedDiagnostics.MaximumDepth;
		Facts.BuildStatus = CachedDiagnostics.Status;
		Facts.bCacheHit = CachedDiagnostics.bCacheHit;
		return Facts;
	}

	auto FTerrainCollisionCoordinator::BuildCollisionGeometry(
		FCollisionGeometryRef& OutGeometry, FTransform& OutWorldTransform) const -> bool
	{
		OutGeometry = {};
		std::string Error;
		if (!Owner.ValidateProperties(Error))
			return SetFailure(ETerrainCollisionStatus::InvalidProperties, std::move(Error));
		if (!Owner.Heightmap)
			return SetFailure(ETerrainCollisionStatus::MissingHeightmap,
				"Terrain collision requires an assigned heightmap.");
		const std::shared_ptr<const FTerrainHeightmapPayload> Payload = Owner.Heightmap->GetPayload();
		if (!Payload || !Payload->HasValidLayout() || Payload->Width < 2 || Payload->Height < 2)
			return SetFailure(ETerrainCollisionStatus::InvalidPayload,
				"Terrain collision requires a valid heightmap with at least two samples on each axis.");
		if (Payload->Width > MaximumTerrainCollisionSamples || Payload->Height > MaximumTerrainCollisionSamples)
			return SetFailure(ETerrainCollisionStatus::ExtentRejected, std::format(
				"Terrain heightmap {}x{} exceeds the T2 collision ceiling of {}x{} samples.",
				Payload->Width, Payload->Height, MaximumTerrainCollisionSamples, MaximumTerrainCollisionSamples));
		const uint64 AssetRevision = Owner.Heightmap->GetRevision();
		if (!CacheMatches(Payload, AssetRevision))
		{
			FCollisionGeometryBuildDiagnostics Diagnostics;
			CachedGeometry = FCollisionGeometryRef::BuildHeightField(
				Payload->Width, Payload->Height, Payload->Samples, Owner.SpacingX, Owner.SpacingY,
				Owner.HeightScale, Owner.HeightOffset, &Diagnostics);
			if (!CachedGeometry.IsValid())
				return SetFailure(ETerrainCollisionStatus::BuildFailed, std::format(
					"Terrain collision build failed with status {}.", static_cast<uint32>(Diagnostics.Status)));
			CachedDiagnostics = Diagnostics;
			CachedPayload = Payload;
			CachedHeightmapRevision = AssetRevision;
			CachedSpacingX = Owner.SpacingX;
			CachedSpacingY = Owner.SpacingY;
			CachedHeightScale = Owner.HeightScale;
			CachedHeightOffset = Owner.HeightOffset;
		}
		OutGeometry = CachedGeometry;
		OutWorldTransform = Owner.GetWorldTransform();
		SetStatus(ETerrainCollisionStatus::Ready);
		return true;
	}
} // namespace Durin
