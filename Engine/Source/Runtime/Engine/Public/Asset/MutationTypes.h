#pragma once

#include "EngineAPI.h"
#include "Asset/Catalog.h"

namespace Durin::Asset
{
	enum class EAssetMutationOperationKind : uint8
	{
		Relocation,
		RedirectorFixup,
		Deletion,
	};

	enum class EAssetMutationTransactionState : uint8
	{
		Empty,
		Prepared,
		Committed,
		Undone,
		RecoveryRequired,
	};

	class FAssetMutationSummary
	{
	public:
		FAssetMutationSummary() = default;
		FAssetMutationSummary(
			EAssetMutationOperationKind InOperationKind,
			uint64 InRegistryRevision,
			std::vector<FAssetPath> InScope
		)
			: OperationKind(InOperationKind)
			, RegistryRevision(InRegistryRevision)
			, Scope(std::move(InScope))
		{
		}

		auto GetOperationKind() const -> EAssetMutationOperationKind
		{
			return OperationKind;
		}
		auto GetRegistryRevision() const -> uint64 { return RegistryRevision; }
		auto GetScope() const -> std::span<const FAssetPath> { return Scope; }

	private:
		EAssetMutationOperationKind OperationKind =
			EAssetMutationOperationKind::Relocation;
		uint64 RegistryRevision = 0;
		std::vector<FAssetPath> Scope;
	};

	struct FAssetMutationResultDetails
	{
		FAssetResult Result;
		EAssetMutationTransactionState State =
			EAssetMutationTransactionState::Empty;
		uint64 RegistryRevision = 0;
		bool bStateRestored = false;
		bool bRecoveryRequired = false;
		std::vector<FAssetPath> RewrittenPaths;
		std::vector<FAssetPath> RetainedPaths;
		std::vector<FAssetPath> DeletedPaths;
		std::vector<FAssetPath> SkippedPaths;
		std::vector<FAssetPath> FailedPaths;
	};

	class FAssetMutationTransaction
	{
	public:
		ENGINE_API auto GetSummary() const -> const FAssetMutationSummary&;
		ENGINE_API auto GetState() const -> EAssetMutationTransactionState;
		ENGINE_API auto GetLastResultDetails() const
			-> FAssetMutationResultDetails;
		ENGINE_API auto Commit() -> FAssetResult;
		ENGINE_API auto Undo() -> FAssetResult;
		ENGINE_API auto Redo() -> FAssetResult;

	private:
		struct FState;
		std::shared_ptr<FState> State;

#if defined(DURIN_ENGINE_ASSET_INTERNAL)
		friend class FAssetMutationCoordinator;
#endif
	};
} // namespace Durin::Asset
