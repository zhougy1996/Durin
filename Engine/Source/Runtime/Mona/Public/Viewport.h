#pragma once

class FViewportClient;
class FRHIViewport;

class KLEE_API FViewport
{
public:
	FViewport(FViewportClient* ViewportClient);

	virtual auto SetInitialSize(FIntPoint InitialSizeXY) -> void;

	virtual auto InitRHIViewport() -> void;

	virtual auto UpdateRHIViewport() -> void;

	auto GetRHIViewport() const -> const TSharedPtr<FRHIViewport>&;

protected:
	FViewportClient* ViewportClient_;

	TSharedPtr<FRHIViewport> RHIViewport_;
};
