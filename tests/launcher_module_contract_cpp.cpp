#include "recomp_launcher_module.h"

/* A separately compiled C++ caller witnesses every field, including nested
 * callback output records, against the C implementation's actual target ABI. */
extern "C" const uint64_t* launcher_v1_cpp_witness(uint32_t* count) {
    static const uint64_t values[] = {
#define RL_WITNESS_STRUCT(type) sizeof(type), alignof(type),
#define RL_WITNESS_FIELD(type, member, signature) offsetof(type, member), sizeof(((type*)0)->member),
#define RL_WITNESS_SIGNATURE(signature)
#include "launcher_module_legacy_witness.inc"
#include "launcher_module_boundary_witness.inc"
#undef RL_WITNESS_STRUCT
#undef RL_WITNESS_FIELD
#undef RL_WITNESS_SIGNATURE
    };
    *count = (uint32_t)(sizeof(values) / sizeof(values[0]));
    return values;
}
