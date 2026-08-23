#pragma once

#include "AssetImportCore.h"

namespace Durin::Asset
{
	class FImporterStore final
	{
	public:
		FImporterStore();
		~FImporterStore();
		FImporterStore(const FImporterStore&) = delete;
		auto operator=(const FImporterStore&) -> FImporterStore& = delete;

		auto Register(std::shared_ptr<IImportProvider> Provider,
			FModuleOwnedCallbackGate OwnerGate, std::string& OutError) -> bool;
		auto Unregister(std::string_view ProviderId) -> bool;
		auto Find(std::string_view ProviderId) const -> FProviderLease;
		auto FindMatching(const FImportSourceRecognition& Source) const
			-> std::vector<FProviderLease>;
		auto GetRevision() const -> uint64;
		auto GetOutstandingLeaseCount(std::string_view ProviderId) const -> uint64;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};

	auto CreateImportPlanInternal(const FImportPlanRequest& Request,
		FImporterStore& Registry) -> FImportPlanResult;


}
