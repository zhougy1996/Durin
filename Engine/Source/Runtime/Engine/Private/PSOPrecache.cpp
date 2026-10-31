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
    struct FEntry;
    struct FRecord
    {
        std::weak_ptr<FEntry> Entry;
        bool Queued = true;
        std::shared_ptr<const std::atomic<ERHIPipelineRequestState>> Outcome;
        int32 CollectorIndex = -1;
        const FVertexFactoryType* VertexFactoryType = nullptr;
    };
    // One canonical identity, terminal success and first-use accounting. Only
    // unresolved admissions/outcomes need individual records.
    struct FEntry
    {
        bool Covered = false, Successful = false, Used = false;
        int32 CollectorIndex = -1;
        const FVertexFactoryType* VertexFactoryType = nullptr;
        std::vector<std::weak_ptr<FRecord>> Queued;
        std::vector<std::shared_ptr<FRecord>> Admitted;
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
        std::unordered_map<FKey, std::shared_ptr<FEntry>, FHash> Records;
        FStatistics Statistics;
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
        auto Compact(FEntry& Entry) -> void
        {
            if (!Entry.Successful) {
                std::erase_if(Entry.Queued, [](const auto& Weak) { const auto Record = Weak.lock(); return !Record || !Record->Queued; });
                std::erase_if(Entry.Admitted, [&](const auto& Record) {
                    const auto State = Record->Outcome->load(std::memory_order_acquire);
                    if (State == ERHIPipelineRequestState::Ready) {
                        Entry.Successful = true;
                        Entry.CollectorIndex = Record->CollectorIndex; Entry.VertexFactoryType = Record->VertexFactoryType;
                    }
                    return State != ERHIPipelineRequestState::Pending;
                });
            }
            if (Entry.Successful) { Entry.Queued.clear(); Entry.Admitted.clear(); }
        }
        template<typename T> auto CheckKey(const T& Initializer, int32 CollectorIndex, const FVertexFactoryType* Factory, bool bRecordUse) -> FCheckResult
        {
            FCheckResult Result{EResult::Untracked, CollectorIndex, Factory};
            if (!Enabled.load()) return Result;
            auto Key = MakeKey(Initializer);
            std::lock_guard Lock(Mutex);
            if (!Enabled.load() || !Key) return Result;
            auto Found = Records.find(*Key);
            if (Found == Records.end() && bRecordUse) Found = Records.emplace(*Key, std::make_shared<FEntry>()).first;
            FEntry* Entry = Found != Records.end() ? Found->second.get() : nullptr;
            if (Entry) Compact(*Entry);
            if (Entry && Entry->Covered) {
                Result.Result = Entry->Successful ? EResult::Hit
                    : !Entry->Queued.empty() || !Entry->Admitted.empty() ? EResult::TooLate : EResult::Failed;
                Result.PSOCollectorIndex = Entry->CollectorIndex;
                Result.VertexFactoryType = Entry->VertexFactoryType;
            } else if (CollectorIndex >= 0 && (!Factory || Factory->SupportsPSOPrecaching())) Result.Result = EResult::Missed;
            if (!bRecordUse || Entry->Used) return Result;
            Entry->Used = true;
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
        Enabled = bEnabled; Records.clear(); Statistics = {};
    }
    auto IsEnabled() -> bool { return Enabled.load(); }
    auto Clear() -> void
    {
        std::lock_guard Lock(Mutex);
        Records.clear(); Statistics = {};
    }
    auto Track(const FPSOPrecacheData& Data) -> std::shared_ptr<FRecord>
    {
        if (!Enabled.load()) return {};
        const auto Key = Data.GetType() == FPSOPrecacheData::EType::Graphics ? MakeKey(Data.GetGraphicsInitializer()) : MakeKey(Data.GetComputeInitializer());
        if (!Key) return {};
        std::lock_guard Lock(Mutex);
        if (!Enabled.load()) return {};
        auto& Entry = Records[*Key];
        if (!Entry) Entry = std::make_shared<FEntry>();
        Compact(*Entry);
        if (Entry->Successful) return {}; // No per-attempt history after successful coverage.
        auto Record = std::make_shared<FRecord>();
        Record->Entry = Entry;
        Record->CollectorIndex = Data.PSOCollectorIndex;
        Record->VertexFactoryType = Data.VertexFactoryType;
        Entry->Covered = true;
        Entry->CollectorIndex = Data.PSOCollectorIndex; Entry->VertexFactoryType = Data.VertexFactoryType;
        Entry->Queued.push_back(Record);
        return Record;
    }
    auto Observe(const std::shared_ptr<FRecord>& Record, std::shared_ptr<const std::atomic<ERHIPipelineRequestState>> Outcome) -> void
    {
        if (!Record) return;
        std::lock_guard Lock(Mutex);
        Record->Queued = false;
        Record->Outcome = std::move(Outcome);
        if (auto Entry = Record->Entry.lock()) {
            if (!Entry->Successful && Record->Outcome
                && std::ranges::none_of(Entry->Admitted, [&](const auto& Other) { return Other->Outcome == Record->Outcome; }))
                Entry->Admitted.push_back(Record);
            Compact(*Entry);
        }
    }
    auto Withdraw(const std::shared_ptr<FRecord>& Record) -> void
    {
        if (!Record) return;
        std::lock_guard Lock(Mutex);
        Record->Queued = false;
        if (auto Entry = Record->Entry.lock()) Compact(*Entry);
    }
    auto Check(const FGraphicsPipelineStateInitializer& Initializer, int32 CollectorIndex, const FVertexFactoryType* Factory) -> FCheckResult
    { return CheckKey(Initializer, CollectorIndex, Factory, false); }
    auto Check(const FComputePipelineStateInitializer& Initializer, int32 CollectorIndex, const FVertexFactoryType* Factory) -> FCheckResult
    { return CheckKey(Initializer, CollectorIndex, Factory, false); }
    auto RecordUse(const FGraphicsPipelineStateInitializer& Initializer, int32 CollectorIndex, const FVertexFactoryType* Factory) -> FCheckResult
    { return CheckKey(Initializer, CollectorIndex, Factory, true); }
    auto RecordUse(const FComputePipelineStateInitializer& Initializer, int32 CollectorIndex, const FVertexFactoryType* Factory) -> FCheckResult
    { return CheckKey(Initializer, CollectorIndex, Factory, true); }
    auto GetStatistics() -> FStatistics
    {
        std::lock_guard Lock(Mutex);
        auto Result = Statistics; Result.Identities = Records.size();
        for (auto& [Key, Entry] : Records) { Compact(*Entry); Result.RetainedAttempts += Entry->Queued.size() + Entry->Admitted.size(); }
        return Result;
    }
}
