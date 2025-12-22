# RegAllocILP: Integer Linear Programming Register Allocation

This note documents the optimization model solved by the ILP-based register allocator implemented in `RegAllocILP.cpp`.

## Scope

The model covers virtual registers that the allocator decides to materialize. Each candidate virtual register may either be assigned to one of its admissible physical registers or marked for spilling. The model does not currently attempt live-range splitting or rematerialization; those are handled in fallback code when the solver declines to produce a placement.

## Decision Variables

For every modelled virtual register `v` and each usable physical register `p` in its allocation order, the allocator introduces a binary assignment variable

```
x[v, p] = 1  if virtual register v is assigned to physical register p
x[v, p] = 0  otherwise
```

If `v` is spillable, a binary spill variable is emitted:

```
s[v] = 1  if v is selected for spilling
s[v] = 0  otherwise
```

Only registers that are live (non-empty use sets) and pass the `shouldAllocateRegister` filter participate in the model.

## Objective Function

The allocator minimizes

```
∑ spill_cost[v] * s[v]
  + ∑ hint_penalty[v, p] * x[v, p]
  + ∑ tie_break[v, p] * x[v, p]
```

- `spill_cost[v]` equals `LiveInterval::weight()` and biases the solver against spilling heavy intervals.
- `hint_penalty[v, p]` is a small negative constant when `p` matches the preferred register reported by `VirtRegMap`, nudging the solver to accept profitable hints.
- `tie_break[v, p]` is a tiny, strictly increasing offset that discourages degeneracy among equivalent assignments and stabilizes solutions across runs.

## Constraints

The model enforces three classes of linear constraints:

1. **Exclusive assignment** — Every modelled virtual register must either receive a unique physical register or spill:
   ```
   ∑_{p∈Phys(v)} x[v, p] + s[v] = 1
   ```
   The spill term is omitted when `v` is non-spillable, forcing a physical assignment instead.

2. **Compatibility** — Physical registers that violate hard constraints (allocation order filters or fixed registers) are excluded a priori, so no extra linear rows are required for them.

3. **Interference avoidance** — For every pair of live intervals `(v, w)` whose live ranges overlap, and for every shared physical register `p`, the allocator adds
   ```
   x[v, p] + x[w, p] ≤ 1
   ```
   ensuring no two interfering intervals occupy the same physical register.

The allocator skips pairs that do not overlap in `LiveInterval::overlaps`, and it avoids constructing constraints for physical registers that are not simultaneously admissible to both virtual registers.

## Solving Strategy

The pass instantiates the CBC mixed-integer solver via OR-Tools and feeds it the model above. If the solver returns an **optimal** or **feasible** solution, the pass materializes the indicated assignments and spill choices directly in the register allocator state. Any other solver status marks the ILP attempt as failed.

## Fallback Plan

When ILP solving fails or OR-Tools support is absent at build time, the pass reverts to the greedy + inline spiller fallback:

- Attempt to grab a free physical register from the allocation order.
- If none are free, spill interfering intervals with lower weight.
- As a last resort, spill the queried virtual register.

This keeps the allocator robust on platforms that lack OR-Tools or on functions whose interference graph is too large for the current model.

## Future Extensions

Potential directions include:

- Adding live-range splitting variables to reduce spill pressure without leaving the ILP framework.
- Modelling rematerialization decisions as zero-cost spill alternatives.
- Switching to the CP-SAT backend to explore larger interference graphs with better heuristics.
