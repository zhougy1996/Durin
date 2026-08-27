#include "StaticMesh/StaticMesh.h"

#include "DObject/Package.h"

#include "Asset.h"
#include "CoreGlobals.h"
#include "DObject/DObjectGlobals.h"
#include "DObject/ObjectLifecycle.h"
#include "DObject/Property.h"
#include "Hash/XxHash.h"
#include "Logging/LogMacros.h"
#include "Math/Operations.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Physics/BodySetup.h"
#include "Serialization/Archive.h"
#include "StaticMesh/StaticMeshDerivedData.h"
#include "StaticMesh/StaticMeshBuild.h"
#include "StaticMesh/StaticMeshRenderStateRecreateContext.h"
#include "StaticMesh/StaticMeshResources.h"
#include "Threading/RunnableThread.h"

#include "RHI.h"
#include "DynamicRHI.h"
#include "RenderingThread.h"

namespace Durin
{
	namespace
	{
		auto SerializeImportedString(FArchive& Ar, std::string& Value) -> void
		{
			Ar << Value;
			if (!Ar.HasError() && Value.size() > 4096)
				Ar.Fail(EArchiveFailureCode::LimitExceeded,
					"StaticMesh imported name exceeds the 4096-byte limit.");
		}

		template<typename TValue, typename FSerializeValue>
		auto SerializeImportedArray(FArchive& Ar, std::vector<TValue>& Values,
			uint64 MaximumCount, FSerializeValue&& SerializeValue) -> void
		{
			uint64 Count = Values.size();
			Ar << Count;
			if (Ar.IsLoading() && !Ar.HasError())
			{
				if (Count > MaximumCount)
				{
					Ar.Fail(EArchiveFailureCode::LimitExceeded,
						"StaticMesh imported array exceeds its element limit.");
					return;
				}
				Values.resize(static_cast<size_t>(Count));
			}
			for (TValue& Value : Values)
			{
				SerializeValue(Ar, Value);
				if (Ar.HasError()) return;
			}
		}

		auto SerializeStaticMeshImportedValue(
			FArchive& Ar, FStaticMeshImportedData& Value) -> void
		{
			uint32 Schema = StaticMeshImportedDataSchemaVersion;
			Ar << Schema;
			if (Ar.IsLoading() && Schema != StaticMeshImportedDataSchemaVersion)
			{
				Ar.Fail(EArchiveFailureCode::InvalidData,
					"StaticMesh imported-data schema is incompatible.");
				return;
			}
			SerializeImportedArray(Ar, Value.MaterialSlots, MaximumMeshMaterialSlots,
				[](FArchive& Inner, FStaticMeshImportedMaterialSlot& Slot) {
					SerializeImportedString(Inner, Slot.Name);
					Inner << Slot.SourceMaterialIndex;
					SerializeImportedString(Inner, Slot.SourceName);
				});
			SerializeImportedArray(Ar, Value.Meshes, 65536,
				[](FArchive& Inner, FStaticMeshImportedMesh& Mesh) {
					SerializeImportedString(Inner, Mesh.Name);
					Inner << Mesh.SourceMaterialIndex;
					auto Vector2 = [](FArchive& A, FVector2f& V) { A << V.x << V.y; };
					auto Vector3 = [](FArchive& A, FVector3f& V) { A << V.x << V.y << V.z; };
					auto Vector4 = [](FArchive& A, FVector4f& V) { A << V.x << V.y << V.z << V.w; };
					SerializeImportedArray(Inner, Mesh.Positions, 50'000'000, Vector3);
					SerializeImportedArray(Inner, Mesh.Normals, 50'000'000, Vector3);
					SerializeImportedArray(Inner, Mesh.Tangents, 50'000'000, Vector4);
					for (auto& UVs : Mesh.UVChannels)
						SerializeImportedArray(Inner, UVs, 50'000'000, Vector2);
					SerializeImportedArray(Inner, Mesh.Colors, 50'000'000, Vector4);
					SerializeImportedArray(Inner, Mesh.Indices, 150'000'000,
						[](FArchive& A, uint32& Index) { A << Index; });
				});
		}

		auto ValidateDecodedStaticMeshImportedData(
			const FStaticMeshImportedData& Value, std::string& OutError) -> bool
		{
			if (Value.MaterialSlots.empty() || Value.MaterialSlots.size() > MaximumMeshMaterialSlots
				|| Value.Meshes.empty() || Value.Meshes.size() > 65536)
			{
				OutError = "StaticMesh canonical geometry has invalid slot or mesh counts.";
				return false;
			}
			std::unordered_set<uint32> SourceMaterials;
			for (const FStaticMeshImportedMaterialSlot& Slot : Value.MaterialSlots)
			{
				if (!SourceMaterials.insert(Slot.SourceMaterialIndex).second)
				{
					OutError = "StaticMesh canonical material source indices must be unique.";
					return false;
				}
			}
			for (const FStaticMeshImportedMesh& Mesh : Value.Meshes)
			{
				if (!SourceMaterials.contains(Mesh.SourceMaterialIndex)
					|| Mesh.Positions.empty() || Mesh.Indices.empty()
					|| Mesh.Indices.size() % 3 != 0
					|| !std::ranges::all_of(Mesh.Positions,
						[](const FVector3f& Position) { return Math::IsFinite(Position); }))
				{
					OutError = "StaticMesh canonical geometry is malformed.";
					return false;
				}
				for (uint32 Index : Mesh.Indices)
					if (Index >= Mesh.Positions.size())
					{
						OutError = "StaticMesh canonical geometry contains an out-of-range index.";
						return false;
					}
			}
			OutError.clear();
			return true;
		}

		auto CheckStaticMeshUpdateThread() -> void
		{
			if (GIsGameThreadIdInitialized) CheckGameThread();
		}

		auto InitializeStaticMeshCandidate(
			FStaticMeshRenderData& Candidate,
			std::string& OutError) -> bool
		{
			Candidate.RecalculateBounds();
			if (GDynamicRHI == nullptr)
			{
				OutError.clear();
				return true;
			}

			std::atomic<bool> bInitialized = false;
			ENQUEUE_RENDER_COMMAND(InitStaticMeshCandidateResources)(
				[&Candidate, &bInitialized](
					FRHICommandListImmediate& CommandList) {
					bInitialized.store(
						Candidate.InitResources(CommandList),
						std::memory_order_release);
				});
			FRenderCommandFence InitFence;
			InitFence.BeginFence();
			InitFence.Wait();
			if (!bInitialized.load(std::memory_order_acquire))
			{
				check(Candidate.GetNumInitializedResources() == 0);
				OutError =
					"Static-mesh candidate resource initialization failed.";
				return false;
			}
			OutError.clear();
			return true;
		}

		auto RetireStaticMeshRenderData(
			std::unique_ptr<FStaticMeshRenderData>& RenderData) -> void
		{
			if (RenderData == nullptr) return;
			if (GDynamicRHI == nullptr)
			{
				check(RenderData->GetNumInitializedResources() == 0);
				return;
			}

			FStaticMeshRenderData* RenderDataToRelease = RenderData.get();
			ENQUEUE_RENDER_COMMAND(ReleaseRetiredStaticMeshResources)(
				[RenderDataToRelease](FRHICommandListImmediate&) {
					RenderDataToRelease->ReleaseResources();
				});
			FRenderCommandFence ReleaseFence;
			ReleaseFence.BeginFence();
			ReleaseFence.Wait();
			check(RenderData->GetNumInitializedResources() == 0);
		}

		constexpr float VectorTolerance = 1.0e-10f;




		auto IsCanonicalStaticMeshHash(std::string_view Hash) -> bool
		{
			return Hash.size() == 32 && std::ranges::all_of(Hash, [](char Character) {
				return Character >= '0' && Character <= '9'
					|| Character >= 'a' && Character <= 'f';
			});
		}


		auto ImportAxisVector(EStaticMeshImportAxis Axis, FVector3f& OutVector, uint32& OutComponent) -> bool
		{
			switch (Axis)
			{
			case EStaticMeshImportAxis::PositiveX: OutVector = FVector3f(1.0f, 0.0f, 0.0f); OutComponent = 0; return true;
			case EStaticMeshImportAxis::NegativeX: OutVector = FVector3f(-1.0f, 0.0f, 0.0f); OutComponent = 0; return true;
			case EStaticMeshImportAxis::PositiveY: OutVector = FVector3f(0.0f, 1.0f, 0.0f); OutComponent = 1; return true;
			case EStaticMeshImportAxis::NegativeY: OutVector = FVector3f(0.0f, -1.0f, 0.0f); OutComponent = 1; return true;
			case EStaticMeshImportAxis::PositiveZ: OutVector = FVector3f(0.0f, 0.0f, 1.0f); OutComponent = 2; return true;
			case EStaticMeshImportAxis::NegativeZ: OutVector = FVector3f(0.0f, 0.0f, -1.0f); OutComponent = 2; return true;
			}
			return false;
		}


	}

	auto FStaticMeshImportedData::CaptureDecodedData(std::string& OutError) -> bool
	{
		if (!ValidateDecodedStaticMeshImportedData(*this, OutError)) return false;
		std::vector<std::byte> Bytes;
		FCanonicalMemoryWriter Ar(Bytes, EArchivePurpose::BulkData);
		SerializeStaticMeshImportedValue(Ar, *this);
		if (Ar.HasError() || Bytes.size() > MaximumStaticMeshImportedDataBytes)
		{
			OutError = Ar.HasError() ? Ar.GetFailure()->Message
				: "StaticMesh canonical geometry exceeds the 1 GiB authored limit.";
			return false;
		}
		if (!Geometry.ReplaceBytes(StaticMeshImportedGeometryPayloadId, Bytes))
		{
			OutError = "StaticMesh canonical geometry could not be retained as authored bulk.";
			return false;
		}
		MaterialSlotCount = static_cast<uint32>(MaterialSlots.size());
		MeshCount = static_cast<uint32>(Meshes.size());
		SchemaVersion = StaticMeshImportedDataSchemaVersion;
		OutError.clear();
		return true;
	}

	auto FStaticMeshImportedData::Decode(std::string& OutError) const
		-> FStaticMeshImportedData
	{
		FStaticMeshImportedData Result;
		const Asset::FBulkData& Bulk = Geometry.GetBulkData();
		if (SchemaVersion != StaticMeshImportedDataSchemaVersion
			|| Bulk.GetDescriptor().PayloadId != StaticMeshImportedGeometryPayloadId
			|| Bulk.GetBytes().empty()
			|| Bulk.GetBytes().size() > MaximumStaticMeshImportedDataBytes)
		{
			OutError = "StaticMesh canonical imported-data header is missing or invalid.";
			return Result;
		}
		FCanonicalMemoryReader Ar(Bulk.GetBytes(), EArchivePurpose::BulkData);
		SerializeStaticMeshImportedValue(Ar, Result);
		if (Ar.HasError() || !RequireArchiveEnd(Ar)
			|| Result.MaterialSlots.size() != MaterialSlotCount
			|| Result.Meshes.size() != MeshCount
			|| !ValidateDecodedStaticMeshImportedData(Result, OutError))
		{
			if (OutError.empty()) OutError = Ar.HasError()
				? Ar.GetFailure()->Message
				: "StaticMesh canonical imported-data counts or payload are invalid.";
			return {};
		}
		OutError.clear();
		return Result;
	}

	auto FStaticMeshImportedData::IsValid() const -> bool
	{
		std::string Error;
		const FStaticMeshImportedData Decoded = Decode(Error);
		return Error.empty() && !Decoded.Meshes.empty();
	}

	auto FStaticMeshImportedData::GetIdentity() const -> FXxHash128
	{
		if (!IsValid()) return {};
		FXxHash128Builder Builder;
		Builder.UpdateValue(SchemaVersion);
		Builder.UpdateValue(MaterialSlotCount);
		Builder.UpdateValue(MeshCount);
		Builder.Update(Geometry.GetBulkData().GetBytes());
		return Builder.Finalize();
	}

	auto FStaticMeshImportSettings::IsValid(std::string* OutError) const -> bool
	{
		FVector3f UnusedVector;
		uint32 ForwardComponent = 0;
		uint32 RightComponent = 0;
		uint32 UpComponent = 0;
		const bool bAxesKnown = ImportAxisVector(ForwardAxis, UnusedVector, ForwardComponent)
			&& ImportAxisVector(RightAxis, UnusedVector, RightComponent)
			&& ImportAxisVector(UpAxis, UnusedVector, UpComponent);
		if (!bAxesKnown)
		{
			if (OutError) *OutError = "The import coordinate system contains an unknown axis.";
			return false;
		}
		if (ForwardComponent == RightComponent || ForwardComponent == UpComponent || RightComponent == UpComponent)
		{
			if (OutError) *OutError = "Forward, Right, and Up must use X, Y, and Z exactly once.";
			return false;
		}
		if (OutError) OutError->clear();
		return true;
	}

	auto FStaticMeshImportSettings::MakeDurin() -> FStaticMeshImportSettings
	{
		return {};
	}

	auto FStaticMeshImportSettings::MakeYUpNegativeZForward() -> FStaticMeshImportSettings
	{
		return {
			.ForwardAxis = EStaticMeshImportAxis::NegativeZ,
			.RightAxis = EStaticMeshImportAxis::PositiveX,
			.UpAxis = EStaticMeshImportAxis::PositiveY
		};
	}

	DStaticMesh::DStaticMesh(const FObjectInitializer& ObjectInitializer)
		: Super(ObjectInitializer)
	{}

	DStaticMesh::~DStaticMesh() = default;

	auto DStaticMesh::GetRenderData() const -> const FStaticMeshRenderData*
	{
		return RenderData.get();
	}

	auto DStaticMesh::GetRenderResourceStatus() const
		-> FStaticMeshRenderResourceStatus
	{
		const uint64 Packed =
			RenderResourceStatus.load(std::memory_order_acquire);
		EStaticMeshRenderResourceReadiness Readiness =
			EStaticMeshRenderResourceReadiness::Unavailable;
		switch (UnpackRenderResourceState(Packed))
		{
		case EStaticMeshRenderResourceState::InitializationQueued:
			Readiness = EStaticMeshRenderResourceReadiness::Queued;
			break;
		case EStaticMeshRenderResourceState::Ready:
			Readiness = EStaticMeshRenderResourceReadiness::Ready;
			break;
		case EStaticMeshRenderResourceState::Failed:
			Readiness = EStaticMeshRenderResourceReadiness::Failed;
			break;
		case EStaticMeshRenderResourceState::Uninitialized:
		case EStaticMeshRenderResourceState::ReleaseQueued:
		case EStaticMeshRenderResourceState::Released:
			break;
		}
		return {
			.Readiness = Readiness,
			.Revision = UnpackRenderResourceRevision(Packed)};
	}

	auto DStaticMesh::GetLOD0LocalBounds() const -> std::optional<FBox>
	{
		if (RenderData == nullptr || RenderData->LODResources.empty())
			return std::nullopt;
		const FBox& Bounds = RenderData->LODResources.front().LocalBounds;
		if (!Bounds.bIsValid
			|| !std::isfinite(Bounds.Min.x)
			|| !std::isfinite(Bounds.Min.y)
			|| !std::isfinite(Bounds.Min.z)
			|| !std::isfinite(Bounds.Max.x)
			|| !std::isfinite(Bounds.Max.y)
			|| !std::isfinite(Bounds.Max.z))
		{
			return std::nullopt;
		}
		const FVector3 Size = Bounds.Max - Bounds.Min;
		if (Size.x <= 0.0 || Size.y <= 0.0 || Size.z <= 0.0)
			return std::nullopt;
		return Bounds;
	}

	auto DStaticMesh::InspectCollision() const -> FStaticMeshCollisionInspection
	{
		FStaticMeshCollisionInspection Result{};
		Result.Mode = EBodySetupCollisionSourceMode::None;
		Result.Policy = EBodySetupCollisionQueryPolicy::SimpleAndComplex;
		Result.BuildStatus = EBodySetupCollisionBuildStatus::None;
		Result.GeometryKind = ECollisionGeometryKind::Primitive;
		Result.BuilderVersion = StaticMeshCollisionBuilderVersion;
		Result.SchemaVersion = StaticMeshCollisionPayloadSchemaVersion;
		if (!BodySetup) return Result;
		Result.Mode = BodySetup->GetCollisionSourceMode();
		Result.Policy = BodySetup->GetCollisionQueryPolicy();
		Result.BuildStatus = BodySetup->GetCollisionBuildStatus();
		Result.BuildRevision = BodySetup->GetCollisionBuildRevision();
		Result.PayloadBytes = BodySetup->GetCollisionPayloadBytes();
		Result.CacheKey = BodySetup->GetCollisionDerivedDataKey();
		Result.Diagnostic = BodySetup->GetCollisionDiagnostic();
		if (RenderData && !RenderData->LODResources.empty())
			Result.SourceTriangles = static_cast<uint32>(
				RenderData->LODResources.front().IndexBuffer.GetIndices().size() / 3);
		FCollisionGeometryRef Geometry;
		if (!BodySetup->BuildSimpleGeometry(Geometry))
			BodySetup->BuildComplexGeometry(Geometry);
		if (!Geometry)
		{
			Result.bRevisionCoherent = Result.Mode == EBodySetupCollisionSourceMode::None;
			return Result;
		}
		Result.GeometryKind = Geometry.GetKind();
		Result.bHasGeometry = true;
		Result.RetainedTriangles = Geometry.GetTriangleCount();
		Result.RemovedTriangles = Result.SourceTriangles >= Result.RetainedTriangles
			? Result.SourceTriangles - Result.RetainedTriangles : 0;
		Result.Nodes = Geometry.GetNodeCount();
		Result.RuntimeBytes = Geometry.GetRetainedBytes();
		FVector3 Minimum;
		FVector3 Maximum;
		if (Geometry.GetLocalBounds(Minimum, Maximum)) Result.Bounds = FBox(Minimum, Maximum);
		Result.bRevisionCoherent = Result.BuildRevision != 0;
		return Result;
	}

	auto DStaticMesh::LoadRenderResourceState() const
		-> EStaticMeshRenderResourceState
	{
		return UnpackRenderResourceState(
			RenderResourceStatus.load(std::memory_order_acquire));
	}

	auto DStaticMesh::PublishRenderResourceState(
		EStaticMeshRenderResourceState State) -> void
	{
		uint64 Current = RenderResourceStatus.load(std::memory_order_acquire);
		for (;;)
		{
			const uint64 Revision = UnpackRenderResourceRevision(Current);
			check(Revision < (std::numeric_limits<uint64>::max()
				>> RenderResourceStateBits));
			const uint64 Desired = PackRenderResourceStatus(State, Revision + 1);
			if (RenderResourceStatus.compare_exchange_weak(
				Current, Desired, std::memory_order_acq_rel))
			{
				return;
			}
		}
	}

	auto DStaticMesh::TryPublishRenderResourceState(
		EStaticMeshRenderResourceState Expected,
		EStaticMeshRenderResourceState State) -> bool
	{
		uint64 Current = RenderResourceStatus.load(std::memory_order_acquire);
		for (;;)
		{
			if (UnpackRenderResourceState(Current) != Expected) return false;
			const uint64 Revision = UnpackRenderResourceRevision(Current);
			check(Revision < (std::numeric_limits<uint64>::max()
				>> RenderResourceStateBits));
			const uint64 Desired = PackRenderResourceStatus(State, Revision + 1);
			if (RenderResourceStatus.compare_exchange_weak(
				Current, Desired, std::memory_order_acq_rel))
			{
				return true;
			}
		}
	}

	auto DStaticMesh::AdvanceRenderResourceRevision() -> void
	{
		PublishRenderResourceState(LoadRenderResourceState());
	}

	auto DStaticMesh::InitResources() -> void
	{
		CheckStaticMeshUpdateThread();
		if (RenderData == nullptr || GDynamicRHI == nullptr) return;

		if (!TryPublishRenderResourceState(
			EStaticMeshRenderResourceState::Uninitialized,
			EStaticMeshRenderResourceState::InitializationQueued))
		{
			return;
		}

#if DURIN_BUILD_DEBUG
		RenderData->SetResourceDebugOwner(GetPackage()
			? FName(GetPackage()->GetPackagePath())
			: FName(std::format(
				"<transient DStaticMesh:{}>", GetName())));
#endif
		FStaticMeshRenderData* RenderDataToInitialize = RenderData.get();
		ENQUEUE_RENDER_COMMAND(InitStaticMeshResources)(
			[this, RenderDataToInitialize](
				FRHICommandListImmediate& CommandList) {
				const EStaticMeshRenderResourceState Result =
					RenderDataToInitialize->InitResources(CommandList)
					? EStaticMeshRenderResourceState::Ready
					: EStaticMeshRenderResourceState::Failed;
				TryPublishRenderResourceState(
					EStaticMeshRenderResourceState::InitializationQueued,
					Result);
			});
	}

	auto DStaticMesh::ReleaseResources() -> void
	{
		CheckStaticMeshUpdateThread();
		const EStaticMeshRenderResourceState State =
			LoadRenderResourceState();
		if (State == EStaticMeshRenderResourceState::Released
			|| State == EStaticMeshRenderResourceState::ReleaseQueued)
		{
			return;
		}

		if (RenderData == nullptr
			|| State == EStaticMeshRenderResourceState::Uninitialized)
		{
			check(RenderData == nullptr
				|| RenderData->GetNumInitializedResources() == 0);
			PublishRenderResourceState(
				EStaticMeshRenderResourceState::Released);
			return;
		}

		PublishRenderResourceState(
			EStaticMeshRenderResourceState::ReleaseQueued);
		FStaticMeshRenderData* RenderDataToRelease = RenderData.get();
		ENQUEUE_RENDER_COMMAND(ReleaseStaticMeshResources)(
			[this, RenderDataToRelease](FRHICommandListImmediate&) {
				RenderDataToRelease->ReleaseResources();
				check(
					RenderDataToRelease->GetNumInitializedResources()
					== 0);
				PublishRenderResourceState(
					EStaticMeshRenderResourceState::Released);
			});
	}

	auto DStaticMesh::GetMaterialSlot(uint32 SlotIndex) const -> const FMeshMaterialSlotDefinition*
	{
		return SlotIndex < MaterialSlots.size() ? &MaterialSlots[SlotIndex] : nullptr;
	}

	auto DStaticMesh::FindMaterialSlot(FName Name) const -> const FMeshMaterialSlotDefinition*
	{
		const auto It = std::ranges::find(MaterialSlots, Name, &FMeshMaterialSlotDefinition::Name);
		return It == MaterialSlots.end() ? nullptr : &*It;
	}

	auto DStaticMesh::GetMaterialIndex(FName Name) const -> std::optional<uint32>
	{
		if (Name.IsNone()) return std::nullopt;
		const auto It = std::ranges::find(MaterialSlots, Name, &FMeshMaterialSlotDefinition::Name);
		if (It == MaterialSlots.end()) return std::nullopt;
		return static_cast<uint32>(std::distance(MaterialSlots.begin(), It));
	}

	auto DStaticMesh::RenameMaterialSlot(uint32 SlotIndex, FName Name, std::string& OutError) -> bool
	{
		if (SlotIndex >= MaterialSlots.size())
		{
			OutError = std::format("Static mesh material slot index {} is out of range.", SlotIndex);
			return false;
		}
		if (Name.IsNone())
		{
			OutError = "Static mesh material slot name cannot be None.";
			return false;
		}
		const auto Existing = std::ranges::find(MaterialSlots, Name, &FMeshMaterialSlotDefinition::Name);
		if (Existing != MaterialSlots.end() && Existing != MaterialSlots.begin() + SlotIndex)
		{
			OutError = std::format("Static mesh material slot name '{}' is already in use.", Name.ToString());
			return false;
		}
		if (MaterialSlots[SlotIndex].Name == Name)
		{
			OutError.clear();
			return true;
		}
		MaterialSlots[SlotIndex].Name = Name;
		if (RenderData && SlotIndex < RenderData->MaterialSlots.size())
			RenderData->MaterialSlots[SlotIndex].Name = Name.ToString();
		MarkPackageDirty();
		OutError.clear();
		return true;
	}

	auto DStaticMesh::CommitRenderDataCandidate(
		std::unique_ptr<FStaticMeshRenderData> InRenderData,
		std::vector<FMeshMaterialSlotDefinition>* InMaterialSlots,
		std::string& OutError,
		bool bBuildAuthoredCollision) -> bool
	{
		CheckStaticMeshUpdateThread();
		if (InRenderData == nullptr)
		{
			OutError = "Static-mesh publication requires render data.";
			return false;
		}
		if (!ValidateStaticMeshLODScreenSizes(
			InRenderData->LODResources, OutError))
		{
			return false;
		}
		InRenderData->RecalculateBounds();
#if DURIN_WITH_EDITOR
		for (FStaticMeshLODResources& LOD : InRenderData->LODResources)
			LOD.RayQueryAcceleration = BuildStaticMeshRayQueryAcceleration(LOD);
#endif
		FCollisionGeometryRef CollisionSimple;
		FCollisionGeometryRef CollisionComplex;
		EBodySetupCollisionBuildStatus CollisionStatus = EBodySetupCollisionBuildStatus::None;
		std::string CollisionKey;
		std::string CollisionDiagnostic;
		uint64 CollisionPayloadBytes = 0;
		const bool bHasAuthoredCollision = bBuildAuthoredCollision && BodySetup
			&& BodySetup->GetCollisionSourceMode() != EBodySetupCollisionSourceMode::None;
		if (bHasAuthoredCollision && !BuildCollisionCandidate(
			*InRenderData,
			BodySetup->GetCollisionSourceMode(),
			BodySetup->GetCollisionQueryPolicy(),
			CollisionSimple,
			CollisionComplex,
			CollisionStatus,
			CollisionKey,
			CollisionDiagnostic,
			CollisionPayloadBytes,
			OutError)) return false;

#if DURIN_BUILD_DEBUG
		const FName DebugOwner = GetPackage()
			? FName(GetPackage()->GetPackagePath())
			: FName(std::format(
				"<transient DStaticMesh:{}>", GetName()));
#endif
		if (RenderData == nullptr)
		{
#if DURIN_BUILD_DEBUG
			InRenderData->SetResourceDebugOwner(DebugOwner);
#endif
			if (InMaterialSlots != nullptr)
			{
				MaterialSlots = std::move(*InMaterialSlots);
			}
			RenderData = std::move(InRenderData);
			RefreshQualifiedBoxBodySetup();
			if (bHasAuthoredCollision)
			{
				const bool bPublished = BodySetup->PublishCollisionGeometry(
					CollisionSimple, CollisionComplex, CollisionStatus,
					std::move(CollisionKey), std::move(CollisionDiagnostic),
					CollisionPayloadBytes);
				check(bPublished);
			}
			PublishRenderResourceState(
				EStaticMeshRenderResourceState::Uninitialized);
			OutError.clear();
			return true;
		}
#if DURIN_BUILD_DEBUG
		InRenderData->SetResourceDebugOwner(DebugOwner);
#endif
		if (!InitializeStaticMeshCandidate(*InRenderData, OutError))
		{
			return false;
		}

		const EStaticMeshRenderResourceState CandidateState =
			GDynamicRHI != nullptr
				? EStaticMeshRenderResourceState::Ready
				: EStaticMeshRenderResourceState::Uninitialized;
		{
			FStaticMeshRenderStateRecreateContext RecreateContext(this);
			std::unique_ptr<FStaticMeshRenderData> OldRenderData =
				std::move(RenderData);
			if (InMaterialSlots != nullptr)
			{
				MaterialSlots = std::move(*InMaterialSlots);
			}
			RenderData = std::move(InRenderData);
			RefreshQualifiedBoxBodySetup();
			if (bHasAuthoredCollision)
			{
				const bool bPublished = BodySetup->PublishCollisionGeometry(
					CollisionSimple, CollisionComplex, CollisionStatus,
					std::move(CollisionKey), std::move(CollisionDiagnostic),
					CollisionPayloadBytes);
				check(bPublished);
			}
			PublishRenderResourceState(CandidateState);
			RetireStaticMeshRenderData(OldRenderData);
		}
		OutError.clear();
		return true;
	}

	auto DStaticMesh::BeginDestroy() -> void
	{
		const EStaticMeshRenderResourceState State =
			LoadRenderResourceState();
		const bool bHasQueuedResourceWork =
			State != EStaticMeshRenderResourceState::Uninitialized
			&& State != EStaticMeshRenderResourceState::Released;
		ReleaseResources();
		if (bHasQueuedResourceWork)
		{
			ReleaseResourcesFence.BeginFence();
		}
		Super::BeginDestroy();
	}

	auto DStaticMesh::IsReadyForFinishDestroy() -> bool
	{
		return ReleaseResourcesFence.IsFenceComplete()
			&& Super::IsReadyForFinishDestroy();
	}

	auto DStaticMesh::FinishDestroy() -> void
	{
		check(ReleaseResourcesFence.IsFenceComplete());
		check(LoadRenderResourceState()
			== EStaticMeshRenderResourceState::Released);
		check(RenderData == nullptr
			|| RenderData->GetNumInitializedResources() == 0);
		AdvanceRenderResourceRevision();
		RenderData.reset();
		Super::FinishDestroy();
	}

	auto DStaticMesh::CreateDebugTriangle(DObject* Outer) -> DStaticMesh*
	{
		DStaticMesh* Mesh = NewObject<DStaticMesh>(Outer, "DebugStaticMesh");
		Mesh->MaterialSlots.push_back({.Name = FName("Default"), .SourceMaterialIndex = 0});
		auto RenderData = std::make_unique<FStaticMeshRenderData>();
		RenderData->MaterialSlots.push_back({"Default", 0});
		FStaticMeshLODResources& LOD = RenderData->LODResources.emplace_back();
		LOD.ScreenSize = GenerateDefaultStaticMeshLODScreenSizes(1).front();
		LOD.VertexBuffers.PositionVertexBuffer.Init({
			FVector3f(-0.65f, -0.45f, 0.0f),
			FVector3f(0.65f, -0.45f, 0.0f),
			FVector3f(0.0f, 0.65f, 0.0f)
		});
		LOD.IndexBuffer.Init({0, 1, 2});
		LOD.VertexBuffers.StaticMeshVertexBuffer.TangentsVertexBuffer.Init(
			std::vector<FVector3f>(
				3, FVector3f(0.0f, 0.0f, 1.0f)),
			std::vector<FVector4f>(
				3, FVector4f(1.0f, 0.0f, 0.0f, 1.0f)));
		LOD.VertexBuffers.StaticMeshVertexBuffer.TexCoordVertexBuffer.Init(
			{}, 3, 0);
		LOD.VertexBuffers.ColorVertexBuffer.Init({}, 3);
		LOD.VertexBuffers.Finalize(
			LOD.NumTexCoords, LOD.bHasColorVertexData);
		LOD.Sections.push_back({"Default", 0, 3, 0, 2, 0, {}});
		std::string PublishError;
		if (!Mesh->CommitRenderDataCandidate(
			std::move(RenderData), nullptr, PublishError))
		{
			DURIN_ERROR(
				"Failed to create debug static mesh: {}",
				PublishError);
			MarkAsGarbage(Mesh);
			return nullptr;
		}
		return Mesh;
	}

	auto DStaticMesh::PublishRenderData(
		std::unique_ptr<FStaticMeshRenderData> InRenderData,
		std::vector<FMeshMaterialSlotDefinition> InMaterialSlots,
		bool bSlotMetadataChanged,
		std::string& OutError) -> bool
	{
		if (!CommitRenderDataCandidate(
			std::move(InRenderData), &InMaterialSlots, OutError))
		{
			return false;
		}
		if (bSlotMetadataChanged)
		{
			MarkPackageDirty();
			Asset::ReportAssetLoadMutation(
				this,
				"Engine.StaticMesh.MaterialSlotsV1",
				"Static mesh material-slot identity metadata was upgraded.",
				Asset::EAssetLoadMutationKind::Upgrade);
		}
		return true;
	}

	auto DStaticMesh::SeedMaterialReconciliationFrom(
		const DStaticMesh& Previous) -> void
	{
		MaterialSlots = Previous.MaterialSlots;
		if (Previous.BodySetup
			&& Previous.BodySetup->GetCollisionSourceMode() != EBodySetupCollisionSourceMode::None)
		{
			if (!BodySetup)
				BodySetup = NewObject<DBodySetup>(this, "BodySetup", GetConstructionPurpose());
			if (BodySetup)
			{
				BodySetup->SetCollisionQueryPolicy(
					Previous.BodySetup->GetCollisionQueryPolicy());
				BodySetup->SetCollisionSourceMode(
					Previous.BodySetup->GetCollisionSourceMode());
			}
		}
	}

	auto DStaticMesh::PublishImportedProduct(
		FStaticMeshBuildProduct Product,
		std::string& OutError) -> bool
	{
		const float PreviousNormalizedSize = NormalizedSize;
		NormalizedSize = Product.NormalizedSize;
		if (!PublishRenderData(
			std::move(Product.RenderData),
			std::move(Product.MaterialSlots),
			Product.bSlotMetadataChanged,
			OutError))
		{
			NormalizedSize = PreviousNormalizedSize;
			return false;
		}
		DerivedDataDiagnostic = {
			.Status = Product.DerivedDataStatus,
			.Key = std::move(Product.DerivedDataKey),
			.Message = std::move(Product.DiagnosticMessage),
			.bSourceImporterInvoked = Product.bSourceImporterInvoked};
		if (Product.bContainsImportedData)
		{
			check(Product.ImportedData.IsValid());
			ImportedData = std::move(Product.ImportedData);
		}
		if (Product.bMarkPackageDirty) MarkPackageDirty();
		OutError.clear();
		return true;
	}

	auto DStaticMesh::PublishAssetImportData(
		AssetImport::DAssetImportData& Value, std::string& OutError) -> bool
	{
		if (Value.GetOuter() != this)
		{
			OutError = "StaticMesh import data must be an owned inner object.";
			return false;
		}
		if (!Value.Validate(OutError)) return false;
		AssetImportData = &Value;
		MarkPackageDirty();
		OutError.clear();
		return true;
	}

	auto DStaticMesh::SetImportedDefaultMaterial(
		uint32 SourceMaterialIndex,
		DMaterialInterface* Material,
		std::string& OutError) -> bool
	{
		const auto Slot = std::ranges::find(
			MaterialSlots,
			SourceMaterialIndex,
			&FMeshMaterialSlotDefinition::SourceMaterialIndex);
		if (Slot == MaterialSlots.end())
		{
			OutError = std::format(
				"Static mesh has no slot for source material {}.", SourceMaterialIndex);
			return false;
		}
		if (std::ranges::find(
			std::next(Slot),
			MaterialSlots.end(),
			SourceMaterialIndex,
			&FMeshMaterialSlotDefinition::SourceMaterialIndex) != MaterialSlots.end())
		{
			OutError = std::format(
				"Static mesh has ambiguous slots for source material {}.", SourceMaterialIndex);
			return false;
		}
		Slot->DefaultMaterial = Material;
		MarkPackageDirty();
		OutError.clear();
		return true;
	}

	auto DStaticMesh::ExchangeImportedState(
		DStaticMesh& Other,
		std::string& OutError) -> bool
	{
		if (&Other == this)
		{
			OutError.clear();
			return true;
		}
		CheckStaticMeshUpdateThread();
		const EStaticMeshRenderResourceState OtherState =
			Other.LoadRenderResourceState();
		const bool bCandidateAlreadyReady =
			OtherState == EStaticMeshRenderResourceState::Ready;
		const bool bCandidateCanInitialize =
			(OtherState == EStaticMeshRenderResourceState::Uninitialized
				|| OtherState ==
					EStaticMeshRenderResourceState::Released)
			&& Other.RenderData != nullptr
			&& Other.RenderData->GetNumInitializedResources() == 0;
		if (Other.RenderData == nullptr
			|| (!bCandidateAlreadyReady
				&& !bCandidateCanInitialize))
		{
			OutError =
				"Static-mesh imported-state exchange requires a detached "
				"ready or uninitialized candidate.";
			return false;
		}

#if DURIN_BUILD_DEBUG
		const FName DebugOwner = GetPackage()
			? FName(GetPackage()->GetPackagePath())
			: FName(std::format(
				"<transient DStaticMesh:{}>", GetName()));
#endif
		if (!bCandidateAlreadyReady)
		{
#if DURIN_BUILD_DEBUG
			Other.RenderData->SetResourceDebugOwner(DebugOwner);
#endif
			if (!InitializeStaticMeshCandidate(
				*Other.RenderData, OutError))
			{
				return false;
			}
		}
		// An already initialized render resource keeps the debug owner it had
		// when initialization was queued. Relabeling it violates the render-
		// resource lifecycle and is not needed for transferring ownership.

		const EStaticMeshRenderResourceState IncomingState =
			bCandidateAlreadyReady || GDynamicRHI != nullptr
				? EStaticMeshRenderResourceState::Ready
				: EStaticMeshRenderResourceState::Uninitialized;
		{
			FStaticMeshRenderStateRecreateContext RecreateContext(this);
			std::unique_ptr<FStaticMeshRenderData> PreviousRenderData =
				std::move(RenderData);
			std::unique_ptr<FStaticMeshRenderData> IncomingRenderData =
				std::move(Other.RenderData);

			std::swap(NormalizedSize, Other.NormalizedSize);
			std::swap(ImportedData, Other.ImportedData);
			std::swap(MaterialSlots, Other.MaterialSlots);
			std::swap(CookedPayload, Other.CookedPayload);
			if (BodySetup && Other.BodySetup)
				BodySetup->ExchangeCollisionState(*Other.BodySetup);
			std::swap(
				DerivedDataDiagnostic,
				Other.DerivedDataDiagnostic);

			RenderData = std::move(IncomingRenderData);
			PublishRenderResourceState(IncomingState);
			RetireStaticMeshRenderData(PreviousRenderData);
			Other.RenderData = std::move(PreviousRenderData);
			Other.PublishRenderResourceState(
				EStaticMeshRenderResourceState::Uninitialized);
		}
		MarkPackageDirty();
		Other.MarkPackageDirty();
		OutError.clear();
		return true;
	}

	auto DStaticMesh::PrepareImportedStateExchange(
		DStaticMesh& Candidate,
		std::string& OutError) -> std::unique_ptr<FStaticMeshImportedStateExchange>
	{
		if (&Candidate == this || !Candidate.RenderData)
		{
			OutError = "Static-mesh imported-state exchange requires a distinct candidate with render data.";
			return nullptr;
		}
		CheckStaticMeshUpdateThread();
		const EStaticMeshRenderResourceState CandidateState =
			Candidate.LoadRenderResourceState();
		if (CandidateState != EStaticMeshRenderResourceState::Ready)
		{
			if ((CandidateState != EStaticMeshRenderResourceState::Uninitialized
					&& CandidateState != EStaticMeshRenderResourceState::Released)
				|| Candidate.RenderData->GetNumInitializedResources() != 0)
			{
				OutError = "Static-mesh candidate cannot be prepared for imported-state exchange.";
				return nullptr;
			}
#if DURIN_BUILD_DEBUG
			Candidate.RenderData->SetResourceDebugOwner(GetPackage()
				? FName(GetPackage()->GetPackagePath())
				: FName("<transient-static-mesh>"));
#endif
			if (!InitializeStaticMeshCandidate(*Candidate.RenderData, OutError))
			{
				if (OutError.empty())
					OutError = "Static-mesh candidate cannot be prepared for imported-state exchange.";
				return nullptr;
			}
			Candidate.PublishRenderResourceState(
				GDynamicRHI ? EStaticMeshRenderResourceState::Ready
					: EStaticMeshRenderResourceState::Uninitialized);
		}
		OutError.clear();
		return std::unique_ptr<FStaticMeshImportedStateExchange>(
			new FStaticMeshImportedStateExchange(*this, Candidate));
	}

	FStaticMeshImportedStateExchange::FStaticMeshImportedStateExchange(
		DStaticMesh& InTarget,
		DStaticMesh& InCandidate)
		: Target(&InTarget), Candidate(&InCandidate) {}

	FStaticMeshImportedStateExchange::~FStaticMeshImportedStateExchange() = default;

	auto FStaticMeshImportedStateExchange::Swap() noexcept -> void
	{
		check(Target && Candidate && Target != Candidate);
		CheckStaticMeshUpdateThread();
		FStaticMeshRenderStateRecreateContext RecreateContext(Target);
		std::swap(Target->NormalizedSize, Candidate->NormalizedSize);
		std::swap(Target->MaterialSlots, Candidate->MaterialSlots);
		std::swap(Target->CookedPayload, Candidate->CookedPayload);
		if (Target->BodySetup && Candidate->BodySetup)
			Target->BodySetup->ExchangeCollisionState(*Candidate->BodySetup);
		std::swap(Target->DerivedDataDiagnostic, Candidate->DerivedDataDiagnostic);
		std::swap(Target->RenderData, Candidate->RenderData);
		Target->RefreshQualifiedBoxBodySetup();
		Candidate->RefreshQualifiedBoxBodySetup();
		const DStaticMesh::EStaticMeshRenderResourceState TargetState =
			Target->LoadRenderResourceState();
		const DStaticMesh::EStaticMeshRenderResourceState CandidateState =
			Candidate->LoadRenderResourceState();
		Target->PublishRenderResourceState(CandidateState);
		Candidate->PublishRenderResourceState(TargetState);
		// Both render-data sets may already be initialized. Their debug owner
		// describes the initialization site and must not be changed after the
		// ownership exchange.
		Target->MarkPackageDirty();
	}

	auto FStaticMeshImportedStateExchange::Commit() noexcept -> void
	{
		if (bCommitted) return;
		Swap();
		bCommitted = true;
	}

	auto FStaticMeshImportedStateExchange::Reverse() noexcept -> void
	{
		if (!bCommitted) return;
		Swap();
		bCommitted = false;
	}

	auto FStaticMeshImportedStateExchange::Finalize() noexcept -> void
	{
		Target = nullptr;
		Candidate = nullptr;
	}

}
