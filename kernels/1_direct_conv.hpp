#pragma once

#include <hip/hip_runtime.h>
#include <hip/hip_vector_types.h>

using elem_t = float;
using vec_elem_t = float4;
const int VECTOR_SIZE = 4;
/*
BLOCK_WIDTH: width of the block
BLOCK_HEIGHT: height of the block
FILTER_WIDTH: width of the filter
FILTER_HEIGHT: height of the filter
MALLOC_FILTER_WIDTH: width of the filter in the actual SMEM
MALLOC_FILTER_HEIGHT: height of the filter in the actual SMEM
MALLOC_BLOCK_WIDTH: width of the block in the actual SMEM (input)
MALLOC_BLOCK_HEIGHT: height of the block in the actual SMEM (input)
*/
template <const int BLOCK_HEIGHT, const int BLOCK_WIDTH,
          const int FILTER_HEIGHT, const int FILTER_WIDTH,
          const int MALLOC_FILTER_HEIGHT, const int MALLOC_FILTER_WIDTH,
          const int MALLOC_BLOCK_HEIGHT, const int MALLOC_BLOCK_WIDTH,
          const int MALLOC_OUT_C>
__global__
    __launch_bounds__(64)
    void
    gpu_direct_conv2d(elem_t *in, elem_t *out, elem_t *filter, int batch_size,
                      int inC, int inH, int inW, int outC, int outH, int outW,
                      int filterH, int filterW) {
  const int block_y = blockIdx.y;
  const int block_x = blockIdx.x;
  const int batch_id = blockIdx.z;                      

  const int thread_y = threadIdx.y;
  const int thread_x = threadIdx.x;

  __shared__ elem_t s_filter[MALLOC_FILTER_HEIGHT][MALLOC_FILTER_WIDTH];
  __shared__ elem_t s_in[MALLOC_BLOCK_HEIGHT][MALLOC_BLOCK_WIDTH];

  float load_reg[VECTOR_SIZE];

  // boundary check
  bool row_boundary = (block_y == gridDim.y - 1);
  bool col_boundary = (block_x == gridDim.x - 1);

  int in_offset = block_y * BLOCK_HEIGHT * inW + block_x * BLOCK_WIDTH + batch_id * inC * inH * inW;
  int in_block_height = BLOCK_HEIGHT + FILTER_HEIGHT - 1;
  int in_block_width = BLOCK_WIDTH + FILTER_WIDTH - 1;

  int extra_row = outH % BLOCK_HEIGHT; // 5
  int extra_col = outW % BLOCK_WIDTH;  // 5

  if (row_boundary) {
    in_block_height = BLOCK_HEIGHT + extra_row + FILTER_HEIGHT - 1;
  }

  if (col_boundary) {
    in_block_width = BLOCK_WIDTH + extra_col + FILTER_WIDTH - 1;
  }

  // if(thread_x == 0 && thread_y == 0){
  //   printf("smem ===> \n");
  //   for(int row=0; row < 9; row++){
  //     for(int col=0; col < 21; col++){
  //       printf("%f ", s_in[row][col]);
  //     }
  //     printf("\n");
  //   }
  // }

  int cur_out_block_height = BLOCK_HEIGHT;
  int cur_out_block_width = BLOCK_WIDTH;

  if (row_boundary) {
    cur_out_block_height = cur_out_block_height + extra_row;
  }

  if (col_boundary) {
    cur_out_block_width = cur_out_block_width + extra_col;
  }


  float out_tile[MALLOC_OUT_C] = {0};

  int out_pos, temp_pos;

  // Another TODO: Tiling also along the channel dimension
  for (int oc = 0; oc < outC; oc++) {
    int out_base = oc * outH * outW + block_y * BLOCK_HEIGHT * outW +
                   block_x * BLOCK_WIDTH + thread_y * outW + thread_x + batch_id * outC * outH * outW;
    for (int ic = 0; ic < inC; ic++) {

      int num_row_wave = in_block_height / BLOCK_HEIGHT;
      int num_col_wave = in_block_width / BLOCK_WIDTH;

      for (int i = 0; i < num_row_wave; i++) {
        for (int j = 0; j < num_col_wave; j++) {
          s_in[thread_y + i * BLOCK_HEIGHT][thread_x + j * BLOCK_WIDTH] =
              in[in_offset + (ic) * (inH * inW) +
                 (thread_y + i * BLOCK_HEIGHT) * inW + thread_x +
                 j * BLOCK_WIDTH];
          // __syncthreads();
        }
        // __syncthreads();
        if ((thread_x + (num_col_wave)*BLOCK_WIDTH) < in_block_width) {
          s_in[thread_y + i * BLOCK_HEIGHT]
              [thread_x + (num_col_wave)*BLOCK_WIDTH] =
                  in[in_offset + (ic) * (inH * inW) +
                     (thread_y + i * BLOCK_HEIGHT) * inW + thread_x +
                     (num_col_wave)*BLOCK_WIDTH];
        }
      }

      // __syncthreads();

      if (thread_y + (num_row_wave)*BLOCK_HEIGHT < in_block_height) {
        for (int j = 0; j < num_col_wave; j++) {
          s_in[thread_y + (num_row_wave)*BLOCK_HEIGHT]
              [thread_x + j * BLOCK_WIDTH] =
                  in[in_offset + (ic) * (inH * inW) +
                     (thread_y + (num_row_wave)*BLOCK_HEIGHT) * inW + thread_x +
                     j * BLOCK_WIDTH];
        }

        if (thread_x + (num_col_wave)*BLOCK_WIDTH < in_block_width) {
          s_in[thread_y + (num_row_wave)*BLOCK_HEIGHT]
              [thread_x + (num_col_wave)*BLOCK_WIDTH] =
                  in[in_offset + (ic) * (inH * inW) +
                     (thread_y + (num_row_wave)*BLOCK_HEIGHT) * inW + thread_x +
                     (num_col_wave)*BLOCK_WIDTH];
        }
      }

      // __syncthreads();

      // if(thread_x == 0 && thread_y == 0 && blockIdx.x == 1 && blockIdx.y ==
      // 0){
      //   printf("smem ===> \n");
      //   for(int row=0; row < 9; row++){
      //     for(int col=0; col < 32; col++){
      //       printf("%f ", s_in[row][col]);
      //     }
      //     printf("\n");
      //   }
      // }
      
      //TODO: this part can be optimized further
      if (FILTER_HEIGHT < BLOCK_HEIGHT) {
        // fetch filter data from global memory to shared memory
        if (thread_y >= 0 && thread_y < FILTER_HEIGHT && thread_x == 0) {
          for (int j = 0; j < FILTER_WIDTH; j++) {
            s_filter[thread_y][j] =
                filter[thread_y * FILTER_WIDTH + j +
                       oc * inC * FILTER_HEIGHT * FILTER_WIDTH +
                       ic * FILTER_HEIGHT * FILTER_WIDTH];
          }
        }
      } else {
        int num_wave = FILTER_HEIGHT / BLOCK_HEIGHT;
        if (thread_x == 0) {
          for (int i = 0; i < num_wave; i++) {
            for (int j = 0; j < FILTER_WIDTH; j++) {
              s_filter[thread_y + i * BLOCK_HEIGHT][j] =
                  filter[(thread_y + i * BLOCK_HEIGHT) * FILTER_WIDTH + j +
                         oc * inC * FILTER_HEIGHT * FILTER_WIDTH +
                         ic * FILTER_HEIGHT * FILTER_WIDTH];
            }
          }

          if (thread_y + num_wave * BLOCK_HEIGHT < FILTER_HEIGHT) {
            for (int j = 0; j < FILTER_WIDTH; j++) {
              s_filter[thread_y + num_wave * BLOCK_HEIGHT][j] =
                  filter[(thread_y + num_wave * BLOCK_HEIGHT) * FILTER_WIDTH +
                         j + oc * inC * FILTER_HEIGHT * FILTER_WIDTH +
                         ic * FILTER_HEIGHT * FILTER_WIDTH];
            }
          }
        }
      }

      __syncthreads();

      float sum = 0.0f;

      for (int i = 0; i < FILTER_HEIGHT; i++) {
        for (int j = 0; j < FILTER_WIDTH; j++) {
          sum += s_in[thread_y + i][thread_x + j] * s_filter[i][j];
        }
      }
      int out_pos = out_base;
      out[out_pos] += sum;

      if (thread_x + BLOCK_WIDTH < cur_out_block_width) {
        float sum = 0.0f;
        for (int i = 0; i < FILTER_HEIGHT; i++) {
          for (int j = 0; j < FILTER_WIDTH; j++) {
            sum +=
                s_in[thread_y + i][thread_x + BLOCK_WIDTH + j] * s_filter[i][j];
          }
        }

        int out_pos = out_base + BLOCK_WIDTH;
        out[out_pos] += sum;
      }

      if (thread_y + BLOCK_HEIGHT < cur_out_block_height) {
        float sum = 0.0f;
        for (int i = 0; i < FILTER_HEIGHT; i++) {
          for (int j = 0; j < FILTER_WIDTH; j++) {
            sum += s_in[thread_y + BLOCK_HEIGHT + i][thread_x + j] *
                   s_filter[i][j];
          }
        }

        int out_pos = out_base + BLOCK_HEIGHT * outW;
        out[out_pos] += sum;

        if (thread_x + BLOCK_WIDTH < cur_out_block_width) {
          float sum = 0.0f;
          for (int i = 0; i < FILTER_HEIGHT; i++) {
            for (int j = 0; j < FILTER_WIDTH; j++) {
              sum += s_in[thread_y + BLOCK_HEIGHT + i]
                         [thread_x + BLOCK_WIDTH + j] *
                     s_filter[i][j];
            }
          }

          int out_pos = out_base + BLOCK_HEIGHT * outW + BLOCK_WIDTH;
          out[out_pos] += sum;
        }
      }

      // __syncthreads();
    }
    // __syncthreads();
  }
}
