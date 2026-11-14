# Memory-manager development

Use only the `nvs/` (NT Virtual Memory Subsystem) and `cc/` implementations. The user retired the old
`mm/`, `vmm_backup/`, `cc_backup/`, pool backing bridge, and legacy debugger
extension from this worktree on 2026-09-20.

Do not restore, copy or adapt the retired implementations or the upstream ReactOS
memory manager, including copies in temporary storage or Git history, and do not
take their internal design as the model. They are not a fallback. You may read
them for one purpose only: to learn the observable behavior and the contracts
(prototypes, bug check codes and their parameters, registry values, semantics).
Record the fact you needed and where you read it, then write the NVS code yourself.
Implement missing behavior in NVS and validate it with host-native
regression tests first. Keep public NT ABI compatibility without importing the
old internal implementation.

Do not spawn subagents, commit, or push unless the user explicitly asks.
Do not add code comments. Preserve unrelated worktree changes.
