// Copyright (c) Meta Platforms, Inc. and affiliates.
// All rights reserved.
//
// This source code is licensed under the license found in the
// LICENSE file in the root directory of this source tree.

#include <stdint.h>
#include <torchao/experimental/ops/library.h>
#include <torchao/experimental/ops/linear_8bit_act_xbit_weight/linear_8bit_act_xbit_weight.h>
#include <torchao/experimental/ops/parallel.h>
#include <algorithm>
#include <cassert>
#include <cstdlib>

namespace torchao::ops::linear_8bit_act_xbit_weight {

PackWeightDataTilingParams get_default_pack_weight_data_tiling_params(
    const UKernelConfig& ukernel_config,
    int n,
    int target_panels_per_thread) {
  TORCHAO_CHECK(n >= 1, "n must be >= 1");
  TORCHAO_CHECK(
      target_panels_per_thread >= 1, "target_panels_per_thread must be >= 1");

  PackWeightDataTilingParams tiling_params;
  int nr = ukernel_config.nr;
  int num_threads = torchao::get_num_threads();
  int numerator = n;
  int denominator = num_threads * target_panels_per_thread;

  // Set nc = ceil(numerator / denominator)
  int nc = (numerator + denominator - 1) / denominator;
  assert(nc >= 1);

  // Replace nc with the next number nr divides
  nc = ((nc + nr - 1) / nr) * nr;
  tiling_params.nc_by_nr = nc / nr;

  return tiling_params;
}

void pack_weight_data_operator(
    const UKernelConfig& ukernel_config,
    const PackWeightDataTilingParams& tiling_params,
    // Outputs
    void* weight_data,
    // Inputs
    int n,
    int k,
    int group_size,
    const int8_t* weight_qvals,
    const float* weight_scales,
    const int8_t* weight_zeros,
    const float* bias) {
  TORCHAO_CHECK(group_size % 16 == 0, "group_size must be a multiple of 16");
  TORCHAO_CHECK(k % group_size == 0, "group_size must divide k");

  bool has_weight_zeros = (weight_zeros != nullptr);
  bool has_bias = (bias != nullptr);

  int nr = ukernel_config.nr;
  int nc = std::min(n, tiling_params.nc_by_nr * ukernel_config.nr);
  int num_nc_panels = (n + nc - 1) / nc;

  torchao::parallel_1d(0, num_nc_panels, [&](int64_t idx) {
    int nc_tile_idx = idx;
    int n_idx = nc_tile_idx * nc;
    int nc_tile_size = std::min(nc, n - n_idx);

    int weight_data_offset = (n_idx / nr) *
        ukernel_config.weight_packing_config.weight_data_size_fn(
            nr, k, group_size, has_weight_zeros, has_bias);
    int weight_qvals_offset = n_idx * k;
    int weight_scales_and_zeros_offset = (n_idx * k / group_size);

    const int8_t* weight_zeros_ptr = nullptr;
    if (weight_zeros != nullptr) {
      weight_zeros_ptr = weight_zeros + weight_scales_and_zeros_offset;
    }
    const float* bias_ptr = nullptr;
    if (bias != nullptr) {
      bias_ptr = bias + n_idx;
    }

    ukernel_config.weight_packing_config.prepare_weight_data_fn(
        (char*)weight_data + weight_data_offset,
        /*n=*/nc_tile_size,
        k,
        group_size,
        weight_qvals + weight_qvals_offset,
        weight_scales + weight_scales_and_zeros_offset,
        weight_zeros_ptr,
        bias_ptr);
  });
}

// This default mimics XNNPACK behavior if target_tiles_per_thread = 5
LinearTilingParams get_default_linear_tiling_params(
    const UKernelConfig& ukernel_config,
    int m,
    int n,
    int target_tiles_per_thread) {
  TORCHAO_CHECK(m >= 1, "m must be >= 1");
  TORCHAO_CHECK(n >= 1, "n must be >= 1");
  TORCHAO_CHECK(
      target_tiles_per_thread >= 1, "target_tiles_per_thread must be >= 1");

  LinearTilingParams tiling_params;
  auto num_threads = torchao::get_num_threads();
  TORCHAO_CHECK(num_threads >= 1, "num_threads must be >= 1");

  tiling_params.mc_by_mr = 1;
  int mc = tiling_params.mc_by_mr * ukernel_config.linear_configs[0].mr;
  int num_mc_panels = (m + mc - 1) / mc;

  int numerator = n * num_mc_panels;
  int denominator = num_threads * target_tiles_per_thread;

  // Set nc = ceil(numerator / denominator)
  int nc = (numerator + denominator - 1) / denominator;
  assert(nc >= 1);

  // Replace nc with next number nr divides
  int nr = ukernel_config.nr;
  nc = ((nc + nr - 1) / nr) * nr;
  assert(nc % nr == 0);
  tiling_params.nc_by_nr = nc / nr;

  assert(tiling_params.mc_by_mr >= 1);
  assert(tiling_params.nc_by_nr >= 1);
  return tiling_params;
}

namespace internal {

inline size_t
get_activation_data_buffer_size_with_tile_schedule_policy_single_mc_parallel_nc(
    const UKernelConfig& ukernel_config,
    const LinearTilingParams& tiling_params,
    int m,
    int k,
    int group_size,
    bool has_weight_zeros) {
  return ukernel_config.linear_configs[0].activation_data_size_fn(
      tiling_params.mc_by_mr * ukernel_config.linear_configs[0].mr,
      k,
      group_size,
      has_weight_zeros);
}

inline size_t
get_activation_data_buffer_size_with_tile_schedule_policy_parallel_mc_parallel_nc(
    const UKernelConfig& ukernel_config,
    const LinearTilingParams& tiling_params,
    int m,
    int k,
    int group_size,
    bool has_weight_zeros) {
  return ukernel_config.linear_configs[0].activation_data_size_fn(
      m, k, group_size, has_weight_zeros);
}

inline void linear_operator_with_tile_schedule_policy_single_mc_parallel_nc(
    const UKernelConfig& ukernel_config,
    const LinearTilingParams& tiling_params,
    char* activation_data_buffer,
    // Outputs
    float* output,
    // Inputs
    int m,
    int n,
    int k,
    int group_size,
    const void* weight_data,
    const float* activations,
    // Ignored if has_clamp = false
    float clamp_min,
    float clamp_max,
    bool has_weight_zeros,
    bool has_bias,
    bool has_clamp) {
  int nr = ukernel_config.nr;
  int mc =
      std::min(m, tiling_params.mc_by_mr * ukernel_config.linear_configs[0].mr);
  int nc = std::min(n, tiling_params.nc_by_nr * nr);
  int num_mc_panels = (m + mc - 1) / mc;
  int num_nc_panels = (n + nc - 1) / nc;
  size_t weight_data_size =
      ukernel_config.weight_packing_config.weight_data_size_fn(
          nr, k, group_size, has_weight_zeros, has_bias);

  for (int mc_tile_idx = 0; mc_tile_idx < num_mc_panels; mc_tile_idx++) {
    int m_idx = mc_tile_idx * mc;
    int mc_tile_size = std::min(mc, m - m_idx);
    int activations_offset = m_idx * k;
    ukernel_config.linear_configs[0].prepare_activation_data_fn(
        activation_data_buffer,
        /*m=*/mc_tile_size,
        k,
        group_size,
        activations + activations_offset,
        has_weight_zeros);

    torchao::parallel_1d(0, num_nc_panels, [&](int64_t idx) {
      int nc_tile_idx = idx;
      int n_idx = nc_tile_idx * nc;
      int nc_tile_size = std::min(nc, n - n_idx);

      int output_offset = m_idx * n + n_idx;
      int weight_data_offset = (n_idx / nr) * weight_data_size;

      ukernel_config.linear_configs[0].kernel_fn(
          output + output_offset,
          /*output_m_stride=*/n,
          /*m=*/mc_tile_size,
          /*n=*/nc_tile_size,
          k,
          group_size,
          /*weight_data=*/(char*)weight_data + weight_data_offset,
          /*activation_data=*/activation_data_buffer,
          clamp_min,
          clamp_max,
          has_weight_zeros,
          has_bias,
          has_clamp);
    });
  }
}

inline void linear_operator_with_tile_schedule_policy_parallel_mc_parallel_nc(
    const UKernelConfig& ukernel_config,
    const LinearTilingParams& tiling_params,
    char* activation_data_buffer,
    // Outputs
    float* output,
    // Inputs
    int m,
    int n,
    int k,
    int group_size,
    const void* weight_data,
    const float* activations,
    float clamp_min,
    float clamp_max,
    bool has_weight_zeros,
    bool has_bias,
    bool has_clamp) {
  int mr = ukernel_config.linear_configs[0].mr;
  int nr = ukernel_config.nr;
  int mc = std::min(m, tiling_params.mc_by_mr * mr);
  int nc = std::min(n, tiling_params.nc_by_nr * nr);
  int num_mc_panels = (m + mc - 1) / mc;
  int num_nc_panels = (n + nc - 1) / nc;

  size_t weight_data_size =
      ukernel_config.weight_packing_config.weight_data_size_fn(
          nr, k, group_size, has_weight_zeros, has_bias);
  size_t activation_data_size =
      ukernel_config.linear_configs[0].activation_data_size_fn(
          mr, k, group_size, has_weight_zeros);

  torchao::parallel_1d(0, num_mc_panels, [&](int64_t idx) {
    int mc_tile_idx = idx;
    int m_idx = mc_tile_idx * mc;
    int mc_tile_size = std::min(mc, m - m_idx);
    int activations_offset = m_idx * k;
    int activation_data_offset = (m_idx / mr) * activation_data_size;

    ukernel_config.linear_configs[0].prepare_activation_data_fn(
        activation_data_buffer + activation_data_offset,
        /*m=*/mc_tile_size,
        k,
        group_size,
        activations + activations_offset,
        has_weight_zeros);
  });

  torchao::parallel_1d(0, num_mc_panels * num_nc_panels, [&](int64_t idx) {
    int mc_tile_idx = idx / num_nc_panels;
    int m_idx = mc_tile_idx * mc;
    int mc_tile_size = std::min(mc, m - m_idx);

    int nc_tile_idx = idx % num_nc_panels;
    int n_idx = nc_tile_idx * nc;
    int nc_tile_size = std::min(nc, n - n_idx);

    int activation_data_offset = (m_idx / mr) * activation_data_size;
    int output_offset = m_idx * n + n_idx;
    int weight_data_offset = (n_idx / nr) * weight_data_size;

    ukernel_config.linear_configs[0].kernel_fn(
        output + output_offset,
        /*output_m_stride=*/n,
        /*m=*/mc_tile_size,
        /*n=*/nc_tile_size,
        k,
        group_size,
        /*weight_data=*/(char*)weight_data + weight_data_offset,
        /*activation_data=*/activation_data_buffer + activation_data_offset,
        clamp_min,
        clamp_max,
        has_weight_zeros,
        has_bias,
        has_clamp);
  });
}
} // namespace internal

void linear_operator(
    const UKernelConfig& ukernel_config,
    const LinearTilingParams& tiling_params,
    LinearTileSchedulingPolicy scheduling_policy,
    char* activation_data_buffer,
    // Outputs
    float* output,
    // Inputs
    int m,
    int n,
    int k,
    int group_size,
    const void* weight_data,
    const float* activations,
    // Ignored if has_clamp = false
    float clamp_min,
    float clamp_max,
    bool has_weight_zeros,
    bool has_bias,
    bool has_clamp) {
  TORCHAO_CHECK(group_size % 16 == 0, "group_size must be a multiple of 16");
  TORCHAO_CHECK(k % group_size == 0, "group_size must divide k");
  switch (scheduling_policy) {
    case LinearTileSchedulingPolicy::single_mc_parallel_nc:
      internal::linear_operator_with_tile_schedule_policy_single_mc_parallel_nc(
          ukernel_config,
          tiling_params,
          activation_data_buffer,
          output,
          m,
          n,
          k,
          group_size,
          weight_data,
          activations,
          clamp_min,
          clamp_max,
          has_weight_zeros,
          has_bias,
          has_clamp);
      break;
    case LinearTileSchedulingPolicy::parallel_mc_parallel_nc:
      internal::
          linear_operator_with_tile_schedule_policy_parallel_mc_parallel_nc(
              ukernel_config,
              tiling_params,
              activation_data_buffer,
              output,
              m,
              n,
              k,
              group_size,
              weight_data,
              activations,
              clamp_min,
              clamp_max,
              has_weight_zeros,
              has_bias,
              has_clamp);
      break;
    default:
      TORCHAO_CHECK(false, "Unimplemented LinearTileSchedulingPolicy");
  }
}

size_t get_activation_data_buffer_size(
    const UKernelConfig& ukernel_config,
    const LinearTilingParams& tiling_params,
    LinearTileSchedulingPolicy scheduling_policy,
    int m,
    int k,
    int group_size,
    bool has_weight_zeros) {
  switch (scheduling_policy) {
    case LinearTileSchedulingPolicy::single_mc_parallel_nc:
      return internal::
          get_activation_data_buffer_size_with_tile_schedule_policy_single_mc_parallel_nc(
              ukernel_config,
              tiling_params,
              m,
              k,
              group_size,
              has_weight_zeros);
    case LinearTileSchedulingPolicy::parallel_mc_parallel_nc:
      return internal::
          get_activation_data_buffer_size_with_tile_schedule_policy_parallel_mc_parallel_nc(
              ukernel_config,
              tiling_params,
              m,
              k,
              group_size,
              has_weight_zeros);
    default:
      TORCHAO_CHECK(false, "Unimplemented LinearTileSchedulingPolicy");
  }
}

} // namespace
  // torchao::ops::linear_8bit_act_xbit_weight
