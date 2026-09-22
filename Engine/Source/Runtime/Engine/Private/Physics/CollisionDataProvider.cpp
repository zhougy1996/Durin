#include "Physics/PhysicsMeshInputTask.h"

namespace Durin
{
	namespace
	{
		class FResidentPhysicsMeshInputTask final : public FPhysicsMeshInputTask
		{
		public:
			FResidentPhysicsMeshInputTask(FTriMeshCollisionData InData, uint64 InBytes)
				: Data(std::move(InData)), Bytes(InBytes) {}
			auto GetWorkingSetBytes() const -> uint64 override { return Bytes; }
			auto Execute(const FAssetBuildTaskContext& Context) const
				-> std::expected<FTriMeshCollisionData, FPhysicsCookFailure> override
			{
				if (Context.IsCancelled()) return std::unexpected(FPhysicsCookFailure::Cancelled(EPhysicsCookStage::Input));
				if (Bytes > Context.MaximumWorkingSetBytes)
					return std::unexpected(FPhysicsCookFailure{"Collision input exceeds the reservation.", EPhysicsCookStage::Input});
				return Data;
			}
		private:
			FTriMeshCollisionData Data;
			uint64 Bytes;
		};
	}

	auto IInterface_CollisionDataProvider::CreatePhysicsMeshInputTask() const
		-> std::expected<std::unique_ptr<FPhysicsMeshInputTask>, FPhysicsCookFailure>
	{
		auto Data = GetPhysicsTriMeshData();
		if (!Data) return std::unexpected(Data.error());
		FAssetBuildMemoryEstimate Memory{512ull * 1024 * 1024, 1024 * 1024};
		if (!Memory.Add(Data->Positions.size(), 512) || !Memory.Add(Data->Indices.size(), 192))
			return std::unexpected(FPhysicsCookFailure{"Collision input exceeds its working-set budget.", EPhysicsCookStage::Input});
		return std::make_unique<FResidentPhysicsMeshInputTask>(std::move(*Data), Memory.Bytes);
	}

}
