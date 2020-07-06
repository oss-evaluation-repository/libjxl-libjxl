// Copyright (c) the JPEG XL Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "tools/butteraugli_pnorm.h"
#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "tools/butteraugli_pnorm.cc"
#include "hwy/foreach_target.h"
//

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <atomic>

#include "jxl/base/compiler_specific.h"
#include "jxl/base/profiler.h"
#include "jxl/color_encoding.h"

//
#include <hwy/before_namespace-inl.h>
namespace jxl {
#include <hwy/begin_target-inl.h>

double ComputeDistanceP(const ImageF& distmap, const ButteraugliParams& params,
                        double p) {
  PROFILER_FUNC;
  // In approximate-border mode, skip pixels on the border likely to be affected
  // by FastGauss' zero-valued-boundary behavior. The border is less than half
  // the largest-diameter kernel (37x37 pixels), and 0 if the image is tiny.
  // NOTE: chosen such that it is vector-aligned.
  size_t border = (params.approximate_border) ? 8 : 0;
  if (distmap.xsize() <= 2 * border || distmap.ysize() <= 2 * border) {
    border = 0;
  }

  const double onePerPixels = 1.0 / (distmap.ysize() * distmap.xsize());
  if (std::abs(p - 3.0) < 1E-6) {
    double sum1[3] = {0.0};

    // Manually aligned storage to avoid asan crash on clang-7 due to
    // unaligned spill.
#if HWY_CAP_DOUBLE
    using T = double;
    const HWY_CAPPED(float, MaxLanes(HWY_FULL(double)())) df;
#else
    using T = float;
#endif
    const HWY_FULL(T) d;
    constexpr size_t N = MaxLanes(HWY_FULL(T)());
    HWY_ALIGN T sum_totals0[N] = {0};
    HWY_ALIGN T sum_totals1[N] = {0};
    HWY_ALIGN T sum_totals2[N] = {0};

    for (size_t y = border; y < distmap.ysize() - border; ++y) {
      const float* JXL_RESTRICT row = distmap.ConstRow(y);

      auto sums0 = Zero(d);
      auto sums1 = Zero(d);
      auto sums2 = Zero(d);

      size_t x = border;
      for (; x + Lanes(d) <= distmap.xsize() - border; x += Lanes(d)) {
#if HWY_CAP_DOUBLE
        const auto d1 = PromoteTo(d, Load(df, row + x));
#else
        const auto d1 = Load(d, row + x);
#endif
        const auto d2 = d1 * d1 * d1;
        sums0 += d2;
        const auto d3 = d2 * d2;
        sums1 += d3;
        const auto d4 = d3 * d3;
        sums2 += d4;
      }

      Store(sums0 + Load(d, sum_totals0), d, sum_totals0);
      Store(sums1 + Load(d, sum_totals1), d, sum_totals1);
      Store(sums2 + Load(d, sum_totals2), d, sum_totals2);

      for (; x < distmap.xsize() - border; ++x) {
        const double d1 = row[x];
        double d2 = d1 * d1 * d1;
        sum1[0] += d2;
        d2 *= d2;
        sum1[1] += d2;
        d2 *= d2;
        sum1[2] += d2;
      }
    }
    double v = 0;
    v += pow(
        onePerPixels * (sum1[0] + GetLane(SumOfLanes(Load(d, sum_totals0)))),
        1.0 / (p * 1.0));
    v += pow(
        onePerPixels * (sum1[1] + GetLane(SumOfLanes(Load(d, sum_totals1)))),
        1.0 / (p * 2.0));
    v += pow(
        onePerPixels * (sum1[2] + GetLane(SumOfLanes(Load(d, sum_totals2)))),
        1.0 / (p * 4.0));
    v /= 3.0;
    return v;
  } else {
    static std::atomic<int> once{0};
    if (once.fetch_add(1, std::memory_order_relaxed) == 0) {
      fprintf(stderr, "WARNING: using slow ComputeDistanceP\n");
    }
    double sum1[3] = {0.0};
    for (size_t y = border; y < distmap.ysize() - border; ++y) {
      const float* JXL_RESTRICT row = distmap.ConstRow(y);
      for (size_t x = border; x < distmap.xsize() - border; ++x) {
        double d2 = std::pow(row[x], p);
        sum1[0] += d2;
        d2 *= d2;
        sum1[1] += d2;
        d2 *= d2;
        sum1[2] += d2;
      }
    }
    double v = 0;
    for (int i = 0; i < 3; ++i) {
      v += pow(onePerPixels * (sum1[i]), 1.0 / (p * (1 << i)));
    }
    v /= 3.0;
    return v;
  }
}

// TODO(lode): take alpha into account when needed
double ComputeDistance2(const ImageBundle& ib1, const ImageBundle& ib2) {
  PROFILER_FUNC;
  // Convert to sRGB - closer to perception than linear.
  const Image3F* srgb1 = &ib1.color();
  Image3F copy1;
  if (!ib1.IsSRGB()) {
    JXL_CHECK(ib1.CopyTo(Rect(ib1), ColorEncoding::SRGB(ib1.IsGray()), &copy1));
    srgb1 = &copy1;
  }
  const Image3F* srgb2 = &ib2.color();
  Image3F copy2;
  if (!ib2.IsSRGB()) {
    JXL_CHECK(ib2.CopyTo(Rect(ib2), ColorEncoding::SRGB(ib2.IsGray()), &copy2));
    srgb2 = &copy2;
  }

  JXL_CHECK(SameSize(*srgb1, *srgb2));

  const HWY_FULL(float) d;
  const size_t N = Lanes(d);
  HWY_ALIGN float sum_total[MaxLanes(d)] = {0.0f};

  double result = 0;
  // Weighted PSNR as in JPEG-XL: chroma counts 1/8 (they compute on YCbCr).
  // Avoid squaring the weight - 1/64 is too extreme.
  const float weights[3] = {1.0f / 8, 6.0f / 8, 1.0f / 8};
  for (size_t c = 0; c < 3; ++c) {
    const auto weight = Set(d, weights[c]);

    for (size_t y = 0; y < srgb1->ysize(); ++y) {
      const float* JXL_RESTRICT row1 = srgb1->ConstPlaneRow(c, y);
      const float* JXL_RESTRICT row2 = srgb2->ConstPlaneRow(c, y);

      auto sums = Zero(d);
      size_t x = 0;
      for (; x + N <= srgb1->xsize(); x += N) {
        const auto diff = Load(d, row1 + x) - Load(d, row2 + x);
        sums += diff * diff * weight;
      }
      // Workaround for clang-7 asan crash if sums is hoisted outside loops
      // (unaligned spill).
      Store(sums + Load(d, sum_total), d, sum_total);

      for (; x < srgb1->xsize(); ++x) {
        const float diff = row1[x] - row2[x];
        result += diff * diff * weights[c];
      }
    }
  }
  const float sum = GetLane(SumOfLanes(Load(d, sum_total)));
  return sum + result;
}

#include <hwy/end_target-inl.h>
}  // namespace jxl
#include <hwy/after_namespace-inl.h>

#if HWY_ONCE
namespace jxl {
HWY_EXPORT(ComputeDistanceP)
double ComputeDistanceP(const ImageF& distmap, const ButteraugliParams& params,
                        double p) {
  return HWY_DYNAMIC_DISPATCH(ComputeDistanceP)(distmap, params, p);
}

HWY_EXPORT(ComputeDistance2)
double ComputeDistance2(const ImageBundle& ib1, const ImageBundle& ib2) {
  return HWY_DYNAMIC_DISPATCH(ComputeDistance2)(ib1, ib2);
}

}  // namespace jxl
#endif
