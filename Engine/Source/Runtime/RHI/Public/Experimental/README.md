# Experimental RHI API

Headers in this directory are public only for selected production or diagnostic
integration while their owning plan remains open. Include paths intentionally
carry the `Experimental/` prefix so unstable dependencies stay visible.

`RHITransition.h` contains the Stage 4 split-barrier transition object and
begin/end contract. See `Documentation/Runtime/Rendering/RHIPublicAPIStability.md`
for the stability contract and promotion rules.
