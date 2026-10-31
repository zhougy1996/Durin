#include "PSOPrecache.h"

namespace Durin
{
	namespace
	{
		auto CanonicalizePrecacheElements(const FVertexDeclarationElementList& Elements)
			-> std::expected<FVertexDeclarationElementList, EPSOPrecacheVertexFactoryError>
		{
			if (!ValidateVertexDeclarationElements(Elements))
				return std::unexpected(EPSOPrecacheVertexFactoryError::InvalidVertexElements);
			FVertexDeclarationElementList Result{};
			bool End = false;
			for (size_t Index = 0; Index < Elements.size(); ++Index)
			{
				if (Elements[Index].Type == EVertexElementType::None) { End = true; continue; }
				// Reject holes rather than silently dropping live attributes after None.
				if (End) return std::unexpected(EPSOPrecacheVertexFactoryError::InvalidVertexElements);
				Result[Index] = Elements[Index];
			}
			return Result;
		}
	}

	auto FPSOPrecacheVertexFactoryData::GetVertexElements(EVertexInputStreamType StreamType) const
		-> std::expected<FVertexDeclarationElementList, EPSOPrecacheVertexFactoryError>
	{
		if (!VertexFactoryType || !VertexFactoryType->SupportsPSOPrecaching())
			return std::unexpected(EPSOPrecacheVertexFactoryError::UnsupportedVertexFactory);
		if (StreamType >= EVertexInputStreamType::Count
			|| (StreamType == EVertexInputStreamType::PositionOnly
				&& !EnumHasAnyFlags(VertexFactoryType->GetFlags(), EVertexFactoryFlags::SupportsPositionOnly)))
			return std::unexpected(EPSOPrecacheVertexFactoryError::UnsupportedStream);
		FVertexDeclarationElementList Elements{};
		const bool Custom = StreamType == EVertexInputStreamType::Default && CustomDefaultVertexElements.has_value();
		if (Custom) Elements = *CustomDefaultVertexElements;
		else
		{
			if (!VertexFactoryType->HasPSOPrecacheVertexFetchElements())
				return std::unexpected(EPSOPrecacheVertexFactoryError::MissingVertexElements);
			VertexFactoryType->GetPSOPrecacheVertexFetchElements(StreamType, Elements);
		}
		if (Elements[0].Type == EVertexElementType::None
			&& !(StreamType == EVertexInputStreamType::Default
				&& EnumHasAnyFlags(VertexFactoryType->GetFlags(), EVertexFactoryFlags::SupportsManualVertexFetch)))
			return std::unexpected(Custom ? EPSOPrecacheVertexFactoryError::InvalidVertexElements
				: EPSOPrecacheVertexFactoryError::UnsupportedStream);
		return CanonicalizePrecacheElements(Elements);
	}

	auto FPSOPrecacheVertexFactoryData::MatchesVertexElements(const FVertexDeclarationElementList& Elements,
		EVertexInputStreamType StreamType) const -> bool
	{
		const auto Expected = GetVertexElements(StreamType);
		const auto Actual = CanonicalizePrecacheElements(Elements);
		return Expected && Actual && *Expected == *Actual;
	}

}

namespace Durin::PSOPrecacheValidation
{
    struct FRecord
    {
        bool Queued = true;
        std::shared_ptr<const std::atomic<ERHIPipelineRequestState>> Outcome;
        int32 CollectorIndex = -1;
        const FVertexFactoryType* VertexFactoryType = nullptr;
    };
    namespace
    {
        using FKey = std::variant<FGraphicsPipelineStateKey, FComputePipelineStateKey>;
        struct FHash
        {
            auto operator()(const FKey& Key) const -> size_t
            {
                return std::visit([](const auto& Value) -> size_t {
                    if constexpr (std::same_as<std::decay_t<decltype(Value)>, FGraphicsPipelineStateKey>)
                        return FGraphicsPipelineStateKeyHasher{}(Value);
                    else return FComputePipelineStateKeyHasher{}(Value);
                }, Key) ^ Key.index();
            }
        };
        std::atomic_bool Enabled = false;
        std::mutex Mutex;
        std::unordered_map<FKey, std::vector<std::shared_ptr<FRecord>>, FHash> Records;
        FStatistics Statistics;
        std::unordered_set<FKey, FHash> Used;
        template<typename T> auto MakeKey(const T& Initializer) -> std::optional<FKey>
        {
            if constexpr (std::same_as<T, FGraphicsPipelineStateInitializer>) {
                auto Key = BuildGraphicsPipelineStateKey(Initializer, nullptr);
                if (Key) return FKey{std::move(*Key)};
            } else {
                auto Key = BuildComputePipelineStateKey(Initializer, nullptr);
                if (Key) return FKey{std::move(*Key)};
            }
            return {};
        }
        template<typename T> auto CheckKey(const T& Initializer, int32 CollectorIndex, const FVertexFactoryType* Factory, bool bRecordUse) -> FCheckResult
        {
            FCheckResult Result{EResult::Untracked, CollectorIndex, Factory};
            if (!Enabled.load()) return Result;
            auto Key = MakeKey(Initializer);
            std::lock_guard Lock(Mutex);
            if (!Enabled.load()) return Result;
            const auto Found = Key ? Records.find(*Key) : Records.end();
            if (Found != Records.end()) {
                bool Ready = false, Pending = false, SourceChosen = false;
                for (const auto& Record : Found->second) {
                    const auto State = Record->Outcome ? Record->Outcome->load(std::memory_order_acquire)
                        : Record->Queued ? ERHIPipelineRequestState::Pending : ERHIPipelineRequestState::Canceled;
                    Ready |= State == ERHIPipelineRequestState::Ready;
                    Pending |= State == ERHIPipelineRequestState::Pending;
                    if (!SourceChosen || State == ERHIPipelineRequestState::Ready) {
                        SourceChosen = true;
                        Result.PSOCollectorIndex = Record->CollectorIndex;
                        Result.VertexFactoryType = Record->VertexFactoryType;
                    }
                }
                Result.Result = Ready ? EResult::Hit : Pending ? EResult::TooLate : EResult::Failed;
            } else if (Key && CollectorIndex >= 0 && (!Factory || Factory->SupportsPSOPrecaching())) Result.Result = EResult::Missed;
            if (!bRecordUse || !Key || !Used.insert(*Key).second) return Result;
            switch (Result.Result) {
            case EResult::Untracked: ++Statistics.Untracked; break;
            case EResult::Missed: ++Statistics.Missed; break;
            case EResult::TooLate: ++Statistics.TooLate; break;
            case EResult::Hit: ++Statistics.Hit; break;
            case EResult::Failed: ++Statistics.Failed; break;
            }
            return Result;
        }
    }
    auto SetEnabled(bool bEnabled) -> void
    {
        std::lock_guard Lock(Mutex);
        Enabled = bEnabled; Records.clear(); Used.clear(); Statistics = {};
    }
    auto IsEnabled() -> bool { return Enabled.load(); }
    auto Clear() -> void
    {
        std::lock_guard Lock(Mutex);
        Records.clear(); Used.clear(); Statistics = {};
    }
    auto Track(const FPSOPrecacheData& Data) -> std::shared_ptr<FRecord>
    {
        if (!Enabled.load()) return {};
        const auto Key = Data.Type == FPSOPrecacheData::EType::Graphics ? MakeKey(Data.GraphicsInitializer) : MakeKey(Data.ComputeInitializer);
        if (!Key) return {};
        std::lock_guard Lock(Mutex);
        if (!Enabled.load()) return {};
        auto Record = std::make_shared<FRecord>();
        Record->CollectorIndex = Data.PSOCollectorIndex;
        Record->VertexFactoryType = Data.VertexFactoryType;
        Records[*Key].push_back(Record);
        return Record;
    }
    auto Observe(const std::shared_ptr<FRecord>& Record, std::shared_ptr<const std::atomic<ERHIPipelineRequestState>> Outcome) -> void
    {
        if (!Record) return;
        std::lock_guard Lock(Mutex);
        Record->Queued = false;
        Record->Outcome = std::move(Outcome);
    }
    auto Withdraw(const std::shared_ptr<FRecord>& Record) -> void
    {
        if (!Record) return;
        std::lock_guard Lock(Mutex);
        Record->Queued = false;
    }
    auto Check(const FGraphicsPipelineStateInitializer& Initializer, int32 CollectorIndex, const FVertexFactoryType* Factory) -> FCheckResult
    { return CheckKey(Initializer, CollectorIndex, Factory, false); }
    auto Check(const FComputePipelineStateInitializer& Initializer, int32 CollectorIndex, const FVertexFactoryType* Factory) -> FCheckResult
    { return CheckKey(Initializer, CollectorIndex, Factory, false); }
    auto RecordUse(const FGraphicsPipelineStateInitializer& Initializer, int32 CollectorIndex, const FVertexFactoryType* Factory) -> FCheckResult
    { return CheckKey(Initializer, CollectorIndex, Factory, true); }
    auto RecordUse(const FComputePipelineStateInitializer& Initializer, int32 CollectorIndex, const FVertexFactoryType* Factory) -> FCheckResult
    { return CheckKey(Initializer, CollectorIndex, Factory, true); }
    auto GetStatistics() -> FStatistics { std::lock_guard Lock(Mutex); return Statistics; }
}
