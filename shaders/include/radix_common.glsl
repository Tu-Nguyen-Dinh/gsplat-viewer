// Shared constants of the GPU radix sort (see src/render/gpu_radix_sort.h).
//
// Each work group handles one BLOCK of 1024 keys: 256 threads x 4 rounds.
// Each pass sorts by one 4-bit digit (16 buckets): 8 passes for 32-bit keys,
// 4 passes for 16-bit keys.
#define RADIX_THREADS 256u
#define RADIX_ROUNDS 4u
#define RADIX_BLOCK (RADIX_THREADS * RADIX_ROUNDS)
#define RADIX_BITS 4u
#define RADIX_BUCKETS 16u

// Number of elements to sort lives on the GPU (written by the preprocess pass).
layout(std430, binding = 7) readonly buffer CountBuffer { uint countData[]; };
uniform uint uCountIndex;   // which uint of CountBuffer holds the count
uniform uint uShift;        // bit offset of the digit sorted in this pass
uniform uint uNumBlocks;

uint elementCount() { return countData[uCountIndex]; }
uint digitOf(uint key) { return (key >> uShift) & (RADIX_BUCKETS - 1u); }
