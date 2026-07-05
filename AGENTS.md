#Coding Guidelines
#General
Data-oriented C99 with C++ deps, SDL GPU graphics/platform
Use SIMD macros from Common/SIMD.h; prefer v128f over float3 for performance
zero stdlib, use SDL and in project functions instead (Math.h, Algorithm.h ...)
AX_WARN/AX_INFO on early returns

#Memory
instead of growable do max limited allocations
Stack: small transient allocations
Static: persistent, <1GB, single instance
Arena (temp <256MB): ArenaPushGlobal() / ArenaPopGlobal()
TLSF/OS: persistent or large allocations
Use functions (Maxf32, etc.) instead of macros
don't null check on minimal allocations

#Code Order
Includes > defines > enums > structs
Externs > globals > statics > public functions
Init at start, cleanup at end of functions

#Notes
Force recompile when editing shader helpers (commonstructs.hlsl, math.hlsl) — Python won't auto-detect changes
leave visual confirmation to me, so don't use renderdoc output png's.
SDL StructuredBuffer limt is 8 RWStructuredBuffer limit is also 8
ternary operator for more than one ? is forbiden use lookup instead