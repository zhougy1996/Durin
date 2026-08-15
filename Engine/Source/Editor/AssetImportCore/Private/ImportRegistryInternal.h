#pragma once

#include "AssetImportCore.h"

namespace Durin::Asset::Import
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

	class FSingleAssetHandlerRegistry final
	{
	public:
		FSingleAssetHandlerRegistry();
		~FSingleAssetHandlerRegistry();
		FSingleAssetHandlerRegistry(const FSingleAssetHandlerRegistry&) = delete;
		auto operator=(const FSingleAssetHandlerRegistry&)
			-> FSingleAssetHandlerRegistry& = delete;

		auto Register(std::shared_ptr<ISingleAssetImportHandler> Handler,
			FModuleOwnedCallbackGate OwnerGate, std::string& OutError) -> bool;
		auto Unregister(std::string_view AssetClassName) -> bool;
		auto Find(std::string_view AssetClassName) const
			-> std::shared_ptr<const ISingleAssetImportHandler>;
		auto GetRevision() const -> uint64;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};

	class FImportRecordHandlerRegistry final
	{
	public:
		FImportRecordHandlerRegistry();
		~FImportRecordHandlerRegistry();
		FImportRecordHandlerRegistry(const FImportRecordHandlerRegistry&) = delete;
		auto operator=(const FImportRecordHandlerRegistry&)
			-> FImportRecordHandlerRegistry& = delete;

		auto Register(std::shared_ptr<IImportRecordHandler> Handler,
			FModuleOwnedCallbackGate OwnerGate, std::string& OutError) -> bool;
		auto Unregister(std::string_view ProviderId) -> bool;
		auto Find(std::string_view ProviderId) const
			-> std::shared_ptr<const IImportRecordHandler>;
		auto GetRevision() const -> uint64;

	private:
		struct FImpl;
		std::unique_ptr<FImpl> Impl;
	};
}
