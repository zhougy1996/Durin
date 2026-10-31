#pragma once

#include "CoreMinimal.h"
#include "DObject/AssetPath.h"
#include "DObject/OverriddenPropertyRestore.h"
#include "Logging/LogMacros.h"

namespace Durin::AssetPrivate
{
	// UE-style loading landmarks, without changing loading ownership or scheduling.
	enum class EObjectLoadPhase : uint8
	{
		ObjectApplication, LoadPackage, ReadPackage, ReadHeader, InspectBulk,
		DecodePackage, LinkerLoad, ValidateSchema, CreateExports, ResolveImports,
		Preload, InitDefaults, CreateClassDefaults, MatchDefaultSubobjects,
		CopyDefaultProperties, SerializeExports, RestoreEditorState,
		ResolveReferences, ValidateLoadedGraph, PostLoad, RestoreCustomVersions,
		CollectCompatibilityEvidence, BuildOverrideLedger, ApplyOverrideLedger, Count
	};
	struct FObjectLoadPhaseTiming
	{
		int64 Inclusive = 0, Children = 0, Dependencies = 0;
		uint64 Calls = 0, Objects = 0, Properties = 0;
	};
	struct FObjectLoadPackageTiming
	{
		FPackagePath Path;
		uint64 Objects = 0, Properties = 0;
		uint64 BindingBuilds = 0, BindingReuses = 0;
		uint64 RestoreFastObjects = 0, RestoreFallbackObjects = 0, RestoreCaptures = 0, RestorePathVisits = 0;
		std::array<uint64, static_cast<size_t>(EOverrideRestoreFallbackReason::Count)> RestoreFallbackReasons{};
		std::array<FObjectLoadPhaseTiming, static_cast<size_t>(EObjectLoadPhase::Count)> Phases{};
	};
	class FScopedObjectLoadPhase;
	// One owner-thread application. Buffer diagnostics until work finishes so
	// logging is excluded. Child package intervals propagate once through parents;
	// deferred validation/PostLoad are attributed to the callback's package.
	class FObjectLoadTimingSession
	{
	public:
		using FClock = int64 (*)();
		explicit FObjectLoadTimingSession(const FPackagePath& InRoot, bool bInLog = true, FClock InClock = Now)
			: Root(InRoot), Clock(InClock), bLog(bInLog), Previous(Current)
		{
			Current = this;
			Started = Clock();
		}
		~FObjectLoadTimingSession()
		{
			const int64 Elapsed = Clock() - Started;
			check(Top == nullptr);
			Current = Previous;
			if (!bLog || Elapsed < 16'000'000) return;
			constexpr std::array<std::string_view, static_cast<size_t>(EObjectLoadPhase::Count)> Names{
				"ObjectApplication", "LoadPackage", "ReadPackage", "ReadHeader", "InspectBulk",
				"DecodePackage", "LinkerLoad", "ValidateSchema", "CreateExports", "ResolveImports",
				"Preload", "InitDefaults", "CreateClassDefaults", "MatchDefaultSubobjects",
				"CopyDefaultProperties", "SerializeExports", "RestoreEditorState",
				"ResolveReferences", "ValidateLoadedGraph", "PostLoad", "RestoreCustomVersions",
				"CollectCompatibilityEvidence", "BuildOverrideLedger", "ApplyOverrideLedger"};
			DURIN_DEBUG_CATEGORY("AssetLoad", "ObjectLoadTiming root={} total_ms={:.3f} packages={}",
				Root.ToString(), Milliseconds(Elapsed), Packages.size());
			for (const auto& Package : Packages)
			{
				int64 Own = 0;
				for (const auto& Phase : Package.Phases) Own += Phase.Inclusive - Phase.Children;
				DURIN_DEBUG_CATEGORY("AssetLoad", "ObjectLoadPackage root={} package={} own_ms={:.3f} objects={} properties={}",
					Root.ToString(), Package.Path.ToString(), Milliseconds(Own), Package.Objects, Package.Properties);
				DURIN_DEBUG_CATEGORY("AssetLoad", "ObjectLoadBindings root={} package={} builds={} reuses={}",
					Root.ToString(), Package.Path.ToString(), Package.BindingBuilds, Package.BindingReuses);
				DURIN_DEBUG_CATEGORY("AssetLoad", "ObjectLoadOverrideRestore root={} package={} fast_objects={} fallback_objects={} full_captures={} path_visits={}",
					Root.ToString(), Package.Path.ToString(), Package.RestoreFastObjects, Package.RestoreFallbackObjects,
					Package.RestoreCaptures, Package.RestorePathVisits);
				for (size_t Reason = 1; Reason < Package.RestoreFallbackReasons.size(); ++Reason)
					if (Package.RestoreFallbackReasons[Reason])
						DURIN_DEBUG_CATEGORY("AssetLoad", "ObjectLoadOverrideFallback root={} package={} reason={} objects={}",
							Root.ToString(), Package.Path.ToString(), Reason, Package.RestoreFallbackReasons[Reason]);
				for (size_t Index = 0; Index < Package.Phases.size(); ++Index)
				{
					const auto& Phase = Package.Phases[Index];
					if (!Phase.Calls) continue;
					DURIN_DEBUG_CATEGORY("AssetLoad", "ObjectLoadPhase root={} package={} phase={} inclusive_ms={:.3f} self_ms={:.3f} dependency_ms={:.3f} exclusive_ms={:.3f} calls={} objects={} properties={} object_visits={} property_visits={}",
						Root.ToString(), Package.Path.ToString(), Names[Index], Milliseconds(Phase.Inclusive),
						Milliseconds(Phase.Inclusive - Phase.Dependencies), Milliseconds(Phase.Dependencies),
						Milliseconds(Phase.Inclusive - Phase.Children), Phase.Calls, Package.Objects, Package.Properties, Phase.Objects, Phase.Properties);
				}
			}
		}
		FObjectLoadTimingSession(const FObjectLoadTimingSession&) = delete;
		auto operator=(const FObjectLoadTimingSession&) -> FObjectLoadTimingSession& = delete;
		auto GetPackages() const -> const std::vector<FObjectLoadPackageTiming>& { return Packages; }
		static auto RecordPackage(const FPackagePath& Path, uint64 Objects, uint64 Properties) -> void
		{
			if (!Current) return;
			auto& Package = Current->Packages[Current->FindPackage(Path)];
			Package.Objects = Objects;
			Package.Properties = Properties;
		}
		static auto RecordBindingWork(const FPackagePath& Path, uint64 Builds, uint64 Reuses) -> void
		{
			if (!Current) return;
			auto& Package = Current->Packages[Current->FindPackage(Path)];
			Package.BindingBuilds += Builds;
			Package.BindingReuses += Reuses;
		}
		static auto RecordOverrideRestoration(const FPackagePath& Path, bool bFast, EOverrideRestoreFallbackReason Reason,
			const FAuthoredOverrideValidationWork& Work) -> void
		{
			if (!Current) return;
			auto& Package = Current->Packages[Current->FindPackage(Path)];
			if (bFast) ++Package.RestoreFastObjects;
			else
			{
				++Package.RestoreFallbackObjects;
				const auto Index = static_cast<size_t>(Reason);
				if (Index < Package.RestoreFallbackReasons.size()) ++Package.RestoreFallbackReasons[Index];
			}
			Package.RestoreCaptures += Work.FullCaptures;
			Package.RestorePathVisits += Work.PathVisits;
		}
	private:
		static auto Now() -> int64
		{
			return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}
		static auto Milliseconds(int64 Nanoseconds) -> double { return static_cast<double>(Nanoseconds) / 1'000'000.0; }
		auto FindPackage(const FPackagePath& Path) -> size_t
		{
			for (size_t Index = 0; Index < Packages.size(); ++Index)
				if (Packages[Index].Path == Path) return Index;
			Packages.push_back({.Path = Path});
			return Packages.size() - 1;
		}
		FPackagePath Root;
		FClock Clock;
		bool bLog;
		int64 Started;
		std::vector<FObjectLoadPackageTiming> Packages;
		FScopedObjectLoadPhase* Top = nullptr;
		FObjectLoadTimingSession* Previous;
		inline static thread_local FObjectLoadTimingSession* Current = nullptr;
		friend class FScopedObjectLoadPhase;
	};

	// self excludes nested packages; exclusive also excludes same-package child
	// phases. Only exclusive values can be added across the phase hierarchy.
	class FScopedObjectLoadPhase
	{
	public:
		FScopedObjectLoadPhase(const FPackagePath& Path, EObjectLoadPhase InPhase, uint64 InObjects = 0, uint64 InProperties = 0)
			: Session(FObjectLoadTimingSession::Current), Phase(InPhase), Objects(InObjects), Properties(InProperties)
		{
			if (!Session) return;
			PackageIndex = Session->FindPackage(Path);
			Parent = std::exchange(Session->Top, this);
			Started = Session->Clock();
		}
		~FScopedObjectLoadPhase()
		{
			if (!Session) return;
			const int64 Elapsed = Session->Clock() - Started;
			check(Session->Top == this);
			Session->Top = Parent;
			auto& Result = Session->Packages[PackageIndex].Phases[static_cast<size_t>(Phase)];
			Result.Inclusive += Elapsed; Result.Children += Children; Result.Dependencies += Dependencies;
			++Result.Calls; Result.Objects += Objects; Result.Properties += Properties;
			if (Parent)
			{
				Parent->Children += Elapsed;
				Parent->Dependencies += Parent->PackageIndex == PackageIndex ? Dependencies : Elapsed;
			}
		}
		FScopedObjectLoadPhase(const FScopedObjectLoadPhase&) = delete;
		auto operator=(const FScopedObjectLoadPhase&) -> FScopedObjectLoadPhase& = delete;
	private:
		FObjectLoadTimingSession* Session;
		EObjectLoadPhase Phase;
		uint64 Objects, Properties;
		size_t PackageIndex = 0;
		FScopedObjectLoadPhase* Parent = nullptr;
		int64 Started = 0, Children = 0, Dependencies = 0;
	};
}
