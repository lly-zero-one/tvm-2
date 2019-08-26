#include <string>
#include <limits>

#include <tvm/attrs.h>
#include <tvm/relay/expr.h>

#include "topi/reduction.h"
#include "topi/tags.h"

#include "quantize.h"

namespace topi {
using namespace tvm;

Array<Tensor> data_int8_quantize(
    const Tensor& data,
    const Tensor& zero_point,
    const Tensor& scale,
    bool is_signed,
    int precision) {
  auto q_min = is_signed ? -(1 << (precision - 1)) : 0;
  auto q_max = is_signed ? ((1 << (precision - 1)) - 1) : (1 << precision) - 1;
  auto target_type = is_signed ? Int(8) : UInt(8);

  auto clamp_output = tvm::compute(
      data->shape,
      [&](Var i, Var j) {
         return tvm::cast(target_type,
            tvm::min(
               tvm::max(tvm::cast(Float(32), zero_point(0)) + data(i, j)/scale(0), q_min),
               q_max
            )
         );
      },
      "tensor",
      "int8_quantize"
      );
  auto k = tvm::reduce_axis(Range(0, data->shape[1]), "k");
  auto data_acc = tvm::compute(
      {data->shape[0]},
      [&](Var i) {
          return tvm::sum(tvm::cast(Int(32), clamp_output(i, k)), {k});
      },
      "tensor",
      "int8_quantize_acc"
      );

  return {clamp_output, data_acc};
}

Array<Tensor> data_int8_mm_dequantize(
    const Tensor& data,
    const Tensor& weight,
    const Tensor& weight_acc,
    const Tensor& data_acc,
    const Tensor& data_scale,
    const Tensor& data_zero_point,
    const double weight_scale,
    const int weight_zero_point) {
  // assume M, K and N, K on input shape
  CHECK(weight->shape.size() == 2);
  auto k = tvm::reduce_axis(Range(0, data->shape[1]), "k");
  auto scale_mul = make_const(Float(32), weight_scale) * data_scale(0);

  auto quantized_mm = tvm::compute(
        {data->shape[0], weight->shape[0]},
        [&](Var i, Var j) {
          return tvm::sum(tvm::cast(Int(32), data(i, k)) * tvm::cast(Int(32), weight(j, k)), {k});
        },
        "tensor",
        "dequantized_mm"
      );
  auto zero_point_mul = weight_zero_point*data_zero_point(0)*(data->shape[1]);

  auto result = tvm::compute(
        {data->shape[0], weight->shape[0]},
        [&](Var i, Var j) {
          return scale_mul*(tvm::cast(Float(32), (quantized_mm(i, j)-data_acc(i)*weight_zero_point-
                            weight_acc(j)*data_zero_point(0) + zero_point_mul)));
        },
        "tensor",
        "mm_dequantize"
      );

  return {result};
}
} // namespace topi
