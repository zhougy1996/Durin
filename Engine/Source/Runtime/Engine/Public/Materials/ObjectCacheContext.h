#pragma once

#include "CoreMinimal.h"

#include "DObject/Object.h"
#include "EngineAPI.h"

namespace Durin
{
	class DMaterialInterface;
	struct FMaterialLoadedQueryDiagnostics;

	// Owns result storage, but does not retain the objects. Consume before GC.
	// Results and pointers must not escape that synchronous scope or enter callbacks
	// scheduled for later. Subsequent queries do not invalidate this array.
	template<typename T>
	class TObjectCacheIterator
	{
	public:
		explicit TObjectCacheIterator(std::vector<T*> InObjects) : Objects(std::move(InObjects)) {}
		class FIterator
		{
		public:
			FIterator(typename std::vector<T*>::const_iterator InCurrent,
				typename std::vector<T*>::const_iterator InEnd) : Current(InCurrent), End(InEnd) { SkipRetired(); }
			auto operator*() const -> T* { return *Current; }
			auto operator++() -> FIterator& { ++Current; SkipRetired(); return *this; }
			friend auto operator==(const FIterator&, const FIterator&) -> bool = default;
		private:
			auto SkipRetired() -> void { while (Current != End && !IsValid(*Current)) ++Current; }
			typename std::vector<T*>::const_iterator Current, End;
		};
		auto begin() const -> FIterator { return {Objects.begin(), Objects.end()}; }
		auto end() const -> FIterator { return {Objects.end(), Objects.end()}; }
	private:
		std::vector<T*> Objects;
	};

	// Game-thread batch snapshot: create after mutations, explicitly share through
	// internal updates, and use a fresh context for any reentrant edit. No global
	// implicit nesting. Consume borrowed results before the next host GC safe point.
	class FObjectCacheContext
	{
	public:
		ENGINE_API FObjectCacheContext();
		ENGINE_API ~FObjectCacheContext();
		FObjectCacheContext(const FObjectCacheContext&) = delete;
		auto operator=(const FObjectCacheContext&) -> FObjectCacheContext& = delete;
		ENGINE_API auto GetDirectMaterialChildren(const DMaterialInterface* Parent)
			-> TObjectCacheIterator<DMaterialInterface>;
		ENGINE_API auto GetMaterialsAffectedByMaterials(std::span<DMaterialInterface* const> Materials)
			-> TObjectCacheIterator<DMaterialInterface>;
		ENGINE_API auto GetMaterialsAffectedByMaterial(const DMaterialInterface* Material)
			-> TObjectCacheIterator<DMaterialInterface>;
		ENGINE_API auto GetDiagnostics() const -> const FMaterialLoadedQueryDiagnostics&;
		// Seal discovery before external notifications. Prepared results remain valid
		// until scope exit; any new discovery must use a fresh context.
		ENGINE_API auto EndDiscovery() -> void;
	private:
		struct FState;
		std::unique_ptr<FState> State;
	};
}
