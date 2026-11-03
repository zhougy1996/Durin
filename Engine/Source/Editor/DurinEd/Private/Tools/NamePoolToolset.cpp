#include "Tools/NamePoolToolset.h"
#include "Misc/Name.h"

namespace Durin
{
	namespace
	{
		auto ShardSummarySchema() -> FToolSchema
		{
			FToolSchema Schema(EToolValueType::Object);
			for (const auto* Name : {"shard_count", "created_entries", "used_slots", "capacity"})
			{
				Schema.Properties.emplace(Name, FToolSchema(EToolValueType::UnsignedInteger));
				Schema.Required.emplace_back(Name);
			}
			return Schema;
		}

		auto SummarizeShards(std::span<const FNamePoolShardStats> Shards) -> FToolValue
		{
			uint64 Created = 0, Used = 0, Capacity = 0;
			for (const auto& Shard : Shards)
			{
				Created += Shard.CreatedEntries;
				Used += Shard.UsedSlots;
				Capacity += Shard.Capacity;
			}
			return FToolValue::FObject{{"shard_count", uint64(Shards.size())},
				{"created_entries", Created}, {"used_slots", Used}, {"capacity", Capacity}};
		}
	}

	auto CreateNamePoolToolset() -> std::unique_ptr<FNativeToolset>
	{
		FToolBinding Binding;
		auto& Descriptor = Binding.Descriptor;
		Descriptor.Name = "stats";
		Descriptor.Description = "Samples process-wide name-pool memory and shard totals. Shards are sampled independently; the snapshot is not globally atomic.";
		Descriptor.Effects = EToolEffect::ReadOnly;
		Descriptor.RetryPolicy = EToolRetryPolicy::Idempotent;
		for (const auto* Name : {"created_entries", "entry_bytes", "allocated_block_bytes", "slot_bytes",
			"active_blocks", "allocated_blocks", "block_size_bytes", "max_blocks"})
		{
			Descriptor.Output.Properties.emplace(Name, FToolSchema(EToolValueType::UnsignedInteger));
			Descriptor.Output.Required.emplace_back(Name);
		}
		Descriptor.Output.Properties.emplace("comparison", ShardSummarySchema());
		Descriptor.Output.Properties.emplace("display", ShardSummarySchema());
		Descriptor.Output.Properties.emplace("message", FToolSchema(EToolValueType::String));
		for (const auto* Name : {"comparison", "display", "message"}) Descriptor.Output.Required.emplace_back(Name);
		Binding.Handler = [](const FToolRequest&, const FToolContext&) -> FToolResult {
			const auto Stats = GetNamePoolStats();
			return {EToolStatus::Succeeded, FToolValue::FObject{
				{"created_entries", Stats.CreatedEntries}, {"entry_bytes", Stats.EntryBytes},
				{"allocated_block_bytes", Stats.AllocatedBlockBytes}, {"slot_bytes", Stats.SlotBytes},
				{"active_blocks", Stats.ActiveBlocks}, {"allocated_blocks", Stats.AllocatedBlocks},
				{"block_size_bytes", Stats.BlockSizeBytes}, {"max_blocks", Stats.MaxBlocks},
				{"comparison", SummarizeShards(Stats.ComparisonShards)},
				{"display", SummarizeShards(Stats.DisplayShards)},
				{"message", "Process-wide snapshot; shards are sampled independently. Byte counts are bytes; entry storage includes headers and alignment. Allocated blocks include spare capacity; hash slots are separate."}}};
		};
		return std::make_unique<FNativeToolset>("core.namepool", "Process-wide name-pool diagnostics.",
			std::vector<FToolBinding>{std::move(Binding)});
	}
}
