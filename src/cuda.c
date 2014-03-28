#include <cuda_runtime.h>

#include <stdint.h>

#include "cuda.h"
#include "sha1.h"

#define gpuErrchk(ans) { gpuAssert((ans), __FILE__, __LINE__); }
inline void gpuAssert(cudaError_t code, char *file, int line)
{
   if (code != cudaSuccess)
   {
      fprintf(stderr,"GPUassert: %s %s %d\n", cudaGetErrorString(code), file, line);
   }
}

uint32_t *d_result[NUM_DEVICES];
uint32_t *result[NUM_DEVICES];

int init_hasher(unsigned char){
    uint32_t i;
    
    for(i = 0; i < NUM_DEVICES; ++i)
    {
        cudaSetDevice(i);
        cudaMalloc(&d_result[i], sizeof(uint32_t));
        cudaMallocHost(&result[i], sizeof(uint32_t));

        cudaDeviceSetCacheConfig(cudaFuncCachePreferL1);
    }

    return 0;
}

void* force_hash(hash_args *args){
    cudaSetDevice(args->device_id);
    
    uint32_t i;
    uint32_t problem_idx;
    SHA_CTX msg_ctx;
    uint32_t words[16];
    uint32_t *block_ptr = (uint32_t *)&args->msg[BUFFER_LENGTH-BLOCK_LENGTH];
    uint8_t difficulty = *args->difficulty;

    SHA1_Init(&msg_ctx);
    SHA1_Update(&msg_ctx, args->msg, BUFFER_LENGTH-BLOCK_LENGTH);

    hash_digest_t h_ctx = { msg_ctx.h0, msg_ctx.h1, msg_ctx.h2,
                            msg_ctx.h3, msg_ctx.h4 };

    for(i=0; i < 16; ++i){
        words[i] = __builtin_bswap32(block_ptr[i]);
    }

    cudaMemset(d_result[args->device_id], 0, sizeof(uint32_t));
    gpuErrchk(copy_constants(words, &difficulty, &h_ctx));

    problem_idx = args->device_id;
    while(!(*args->stop)){
        force_kernel(d_result[args->device_id], problem_idx);

        cudaMemcpy(result[args->device_id], d_result[args->device_id], sizeof(uint32_t),
                        cudaMemcpyDeviceToHost);
        if(*(result[args->device_id])){
            args->found = 1;
            block_ptr[12] = __builtin_bswap32((*(result[args->device_id]))-1);
            block_ptr[11] = __builtin_bswap32(problem_idx);
            return NULL;
        }
            
        problem_idx += NUM_DEVICES;
    }

    return NULL;
}

int free_hasher(){
    uint32_t i;
    
    for(i = 0; i < NUM_DEVICES; ++i)
    {
        cudaSetDevice(i);
        cudaFree(d_result[i]);
    }

    return 0;
}
