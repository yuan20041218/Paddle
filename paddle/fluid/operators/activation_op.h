/* Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
    http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#pragma once
#include <glog/logging.h>
#include <algorithm>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <cmath>
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif

#include "paddle/fluid/framework/eigen.h"
#include "paddle/fluid/framework/op_registry.h"
#include "paddle/fluid/operators/detail/safe_ref.h"
#include "paddle/fluid/operators/math/blas.h"
#include "paddle/fluid/platform/float16.h"

#ifdef PADDLE_WITH_MKLDNN
#include "paddle/fluid/platform/mkldnn_helper.h"
#endif

namespace paddle {
namespace operators {

/* Use ugly global variable, for the using in python layer side
   Please refer to the layer_helper.py and get the details.
 */
static std::unordered_set<std::string> InplaceOpSet = {
    "sigmoid", "exp",        "relu",  "tanh",      "sqrt",        "ceil",
    "floor",   "reciprocal", "relu6", "soft_relu", "hard_sigmoid"};

static bool IsInplace(const std::string& op) {
  bool inplace = InplaceOpSet.count(op);
  // for op_grad
  const int kGradSuffixLen = 4;
  if (op.size() > kGradSuffixLen &&
      op.compare(op.size() - kGradSuffixLen - 1, kGradSuffixLen, "grad")) {
    inplace =
        InplaceOpSet.count(op.substr(0, op.size() - (kGradSuffixLen + 1)));
  }
  return inplace;
}

/* The following operator can be used to process SelectedRows, because the
 * output of those operator for zero is zero too.
 */
static std::unordered_set<std::string> CanBeUsedBySelectedRows = {
    "abs", "abs_grad", "square", "square_grad", "sqrt", "sqrt_grad"};

inline void ExtractActivationTensor(const framework::ExecutionContext& context,
                                    const framework::Tensor** X,
                                    framework::Tensor** Out) {
  auto x_var = context.InputVar("X");
  auto out_var = context.OutputVar("Out");
  PADDLE_ENFORCE(x_var != nullptr,
                 "Cannot get input Variable X, variable name = %s",
                 context.op().Input("X"));
  PADDLE_ENFORCE(out_var != nullptr,
                 "Cannot get output Variable Out, variable name = %s",
                 context.op().Output("Out"));
  if (CanBeUsedBySelectedRows.count(context.op().Type())) {
    *X = paddle::framework::GetLoDTensorOrSelectedRowsValueFromVar(*x_var);
    *Out = paddle::framework::GetMutableLoDTensorOrSelectedRowsValueFromVar(
        out_var);
  } else {
    *X = context.Input<framework::Tensor>("X");
    *Out = context.Output<framework::Tensor>("Out");
  }

  PADDLE_ENFORCE(*Out != nullptr,
                 "Cannot get output tensor Out, variable name = %s",
                 context.op().Output("Out"));
}

inline void ExtractActivationGradTensor(
    const framework::ExecutionContext& context, const framework::Tensor** X,
    const framework::Tensor** Out, const framework::Tensor** dOut,
    framework::Tensor** dX) {
  auto out_var = context.InputVar("Out");
  auto out_grad_var = context.InputVar(framework::GradVarName("Out"));
  auto x_grad_var = context.OutputVar(framework::GradVarName("X"));
  PADDLE_ENFORCE(out_var != nullptr,
                 "Cannot get input Variable Out, variable name = %s",
                 context.op().Input("Out"));
  PADDLE_ENFORCE(out_grad_var != nullptr,
                 "Cannot get input Variable %s, variable name = %s",
                 framework::GradVarName("Out"),
                 context.op().Input(framework::GradVarName("Out")));
  PADDLE_ENFORCE(x_grad_var != nullptr,
                 "Cannot get output Variable %s, variable name = %s",
                 framework::GradVarName("X"),
                 context.op().Output(framework::GradVarName("X")));

  if (CanBeUsedBySelectedRows.count(context.op().Type())) {
    *Out = paddle::framework::GetLoDTensorOrSelectedRowsValueFromVar(*out_var);
    *dOut = paddle::framework::GetLoDTensorOrSelectedRowsValueFromVar(
        *out_grad_var);
    *dX = paddle::framework::GetMutableLoDTensorOrSelectedRowsValueFromVar(
        x_grad_var);
  } else {
    *Out = context.Input<framework::Tensor>("Out");
    *dOut = context.Input<framework::Tensor>(framework::GradVarName("Out"));
    *dX = context.Output<framework::Tensor>(framework::GradVarName("X"));
  }
  PADDLE_ENFORCE(*dX != nullptr,
                 "Cannot get output tensor %s, variable name = %s",
                 framework::GradVarName("X"),
                 context.op().Output(framework::GradVarName("X")));

  bool inplace = IsInplace(context.op().Type());
  if (!inplace) {
    auto x_var = context.InputVar("X");
    PADDLE_ENFORCE(x_var != nullptr,
                   "Cannot get input tensor X, variable name = %s",
                   context.op().Input("X"));
    if (CanBeUsedBySelectedRows.count(context.op().Type())) {
      *X = paddle::framework::GetLoDTensorOrSelectedRowsValueFromVar(*x_var);
    } else {
      *X = context.Input<framework::Tensor>("X");
    }
  } else {
    VLOG(10) << " Inplace activation of Op : " << context.op().Type();
    *X = *dX;
  }
}

template <typename DeviceContext, typename Functor>
class ActivationKernel
    : public framework::OpKernel<typename Functor::ELEMENT_TYPE> {
 public:
  using T = typename Functor::ELEMENT_TYPE;

  void Compute(const framework::ExecutionContext& context) const override {
    const framework::Tensor* X = nullptr;
    framework::Tensor* Out = nullptr;
    ExtractActivationTensor(context, &X, &Out);
    Out->mutable_data<T>(context.GetPlace());

    auto x = framework::EigenVector<T>::Flatten(detail::Ref(X));
    auto out = framework::EigenVector<T>::Flatten(detail::Ref(Out));
    auto* place =
        context.template device_context<DeviceContext>().eigen_device();
    Functor functor;

    auto attrs = functor.GetAttrs();
    for (auto& attr : attrs) {
      *attr.second = context.Attr<float>(attr.first);
    }
    functor(*place, x, out);
  }
};

template <typename DeviceContext, typename Functor>
class ActivationGradKernel
    : public framework::OpKernel<typename Functor::ELEMENT_TYPE> {
 public:
  using T = typename Functor::ELEMENT_TYPE;
  void Compute(const framework::ExecutionContext& context) const override {
    const framework::Tensor *X, *Out, *dOut;
    framework::Tensor* dX = nullptr;
    X = Out = dOut = nullptr;
    ExtractActivationGradTensor(context, &X, &Out, &dOut, &dX);
    dX->mutable_data<T>(context.GetPlace());
    auto dout = framework::EigenVector<T>::Flatten(detail::Ref(dOut));
    auto out = framework::EigenVector<T>::Flatten(detail::Ref(Out));
    auto dx = framework::EigenVector<T>::Flatten(detail::Ref(dX));
    auto x = framework::EigenVector<T>::Flatten(detail::Ref(X));
    auto* place =
        context.template device_context<DeviceContext>().eigen_device();
    Functor functor;
    auto attrs = functor.GetAttrs();
    for (auto& attr : attrs) {
      *attr.second = context.Attr<float>(attr.first);
    }
    functor(*place, x, out, dout, dx);
  }
};

template <typename T>
struct BaseActivationFunctor {
  using ELEMENT_TYPE = T;

  using AttrPair = std::vector<std::pair<const char*, float*>>;

  AttrPair GetAttrs() { return AttrPair(); }

  /* NOTE(*): Output reuse X memory if X is not dependented by its Gradient.
     For example, sigmoid op's gradient didn't involve x, so its output can
     reuse
     input memory. But abs op's gradient use x, it can not be inplaced.
     gradient did use x.
   */
  bool Inplace() const { return false; }
};

// sigmoid(x) = 1 / (1 + exp(-x))
template <typename T>
struct SigmoidFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    out.device(d) = static_cast<T>(1) / (static_cast<T>(1) + (-x).exp());
  }
};

template <typename T>
struct SigmoidGradFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    dx.device(d) = dout * out * (static_cast<T>(1) - out);
  }
};

// Originally: logsigmoid(x) = -log (1 + exp(-x))
// For numerical stability, we can use the log-sum-exp trick:
// https://hips.seas.harvard.edu/blog/2013/01/09/computing-log-sum-exp/
// We can rewrite the above equation as:
// out = -log( exp(0) + exp(-x)) [since exp(0) = 1]
//   = -log( exp(max(-x, 0) - max(-x, 0)) + exp(-x + max(-x, 0) - max(-x, 0)))
//   = -log( exp(max(-x, 0)) * exp(-max(-x, 0)) - exp(max(-x, 0)) * exp(-x -
//           max(-x, 0)))
//   = -log( exp(max(-x, 0)) * (exp(-max(-x, 0)) + exp(-x - max(-x, 0))))
//   = -log( exp(max(-x, 0)) - log(exp(-max(-x, 0)) + exp(-x - max(-x, 0)))
//
// Hence, logsigmoid(x) = - (max(-x, 0) + log(exp(-max(-x, 0))
// + exp(-x - max(-x, 0))))
template <typename T>
struct LogSigmoidFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    auto temp = (-x).cwiseMax(static_cast<T>(0));  // temp = max(-x, 0)
    out.device(d) = -temp - (((-temp).exp() + (-x - temp).exp()).log());
  }
};

// Originally: f' = exp(-x) / (1 + exp(-x))
// For numerical stability: f' = exp(-x - max(-x, 0)) / (exp(-max(-x, 0)) +
// exp(-x - max(-x, 0)))
template <typename T>
struct LogSigmoidGradFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    auto temp = (-x).cwiseMax(static_cast<T>(0));  // temp = max(-x, 0)
    dx.device(d) =
        dout * ((-x - temp).exp() / ((-temp).exp() + (-x - temp).exp()));
  }
};

// exp(x) = e^x
template <typename T>
struct ExpFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    out.device(d) = x.exp();
  }
};

template <typename T>
struct ExpGradFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    dx.device(d) = dout * out;
  }
};

// relu(x) = max(x, 0)
template <typename T>
struct ReluFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    out.device(d) = x.cwiseMax(static_cast<T>(0));
  }
};

template <typename T>
struct ReluGradFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    dx.device(d) = dout * (out > static_cast<T>(0)).template cast<T>();
  }
};

// gelu(x) = 0.5 * x *  (1 + erf(x / sqrt(2)))
template <typename T>
struct GeluFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
// Because the execute or device context can not be deliver here, it keep the
// marco for NVCC.
#if defined(PADDLE_WITH_MKLML) && !defined(_WIN32) && !defined(__APPLE__) && \
    !defined(__OSX__) && !defined(PADDLE_WITH_CUDA)
    auto x_data = x.data();
    auto out_data = out.data();
    int n = std::min(x.size(), out.size());

    std::memset(out_data, 0, n * sizeof(T));
    math::CBlas<T>::AXPY(n, static_cast<T>(M_SQRT1_2), x_data, 1, out_data, 1);
    math::CBlas<T>::VMERF(n, out_data, out_data, VML_LA);
    for (int i = 0; i < n; i++) {
      out_data[i] += static_cast<T>(1);
    }
    math::CBlas<T>::VMUL(n, x_data, out_data, out_data);
    for (int i = 0; i < n; i++) {
      out_data[i] *= static_cast<T>(0.5);
    }
#else
    auto temp = (x * static_cast<T>(M_SQRT1_2)).erf();
    out.device(d) = x * static_cast<T>(0.5) * (static_cast<T>(1) + temp);
#endif
  }
};

template <typename T>
struct GeluGradFunctor : BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    auto first = static_cast<T>(0.5) *
                 (static_cast<T>(1) + ((x * static_cast<T>(M_SQRT1_2)).erf()));

    auto second = static_cast<T>(0.5 * M_2_SQRTPI * M_SQRT1_2) * x *
                  (-static_cast<T>(0.5) * x.square()).exp();
    dx.device(d) = dout * (first + second);
  }
};

// tanh(x) = (exp(x) - exp(-x)) / (exp(x) + exp(-x))
template <typename T>
struct TanhFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    out.device(d) = x.tanh();
  }
};

template <typename T>
struct TanhGradFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    dx.device(d) = dout * (static_cast<T>(1) - out * out);
  }
};

// tanhshrink(x) = x - tanh(x)
// where tanh(x) = (exp(x) - exp(-x)) / (exp(x) + exp(-x))
template <typename T>
struct TanhShrinkFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    out.device(d) = x - x.tanh();
  }
};

template <typename T>
struct TanhShrinkGradFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    dx.device(d) = dout * (x.tanh() * x.tanh());
  }
};

// tanhshrink(x) = x - tanh(x)
// where tanh(x) = (exp(x) - exp(-x)) / (exp(x) + exp(-x))
template <typename T>
struct HardShrinkFunctor : public BaseActivationFunctor<T> {
  float threshold;

  typename BaseActivationFunctor<T>::AttrPair GetAttrs() {
    return {{"threshold", &threshold}};
  }
  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    auto temp1 = (x < static_cast<T>(threshold * -1)).template cast<T>().eval();
    auto temp2 = (x > static_cast<T>(threshold)).template cast<T>().eval();
    out.device(d) = x * (temp1 + temp2);
  }
};

template <typename T>
struct HardShrinkGradFunctor : public BaseActivationFunctor<T> {
  float threshold;

  typename BaseActivationFunctor<T>::AttrPair GetAttrs() {
    return {{"threshold", &threshold}};
  }

  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    auto temp1 = (x < static_cast<T>(threshold * -1)).template cast<T>().eval();
    auto temp2 = (x > static_cast<T>(threshold)).template cast<T>().eval();
    dx.device(d) = dout * (temp1 + temp2).template cast<T>();
  }
};

// softshrink(x) = x - lambda, if x > lambda; x + lambda, if x < -lambda; 0
// otherwise
template <typename T>
struct SoftShrinkFunctor : public BaseActivationFunctor<T> {
  float lambda;
  typename BaseActivationFunctor<T>::AttrPair GetAttrs() {
    return {{"lambda", &lambda}};
  }

  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    auto lambdaT = static_cast<T>(lambda);
    auto temp1 = (x > lambdaT).template cast<T>().eval();
    auto temp2 = (x < -lambdaT).template cast<T>().eval();
    out.device(d) = temp1 * (x - lambdaT) + temp2 * (x + lambdaT);
  }
};

template <typename T>
struct SoftShrinkGradFunctor : public BaseActivationFunctor<T> {
  float lambda;
  typename BaseActivationFunctor<T>::AttrPair GetAttrs() {
    return {{"lambda", &lambda}};
  }
  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    auto lambdaT = static_cast<T>(lambda);
    auto temp1 = (x > lambdaT).template cast<T>().eval();
    auto temp2 = (x < -lambdaT).template cast<T>().eval();
    dx.device(d) = dout * (temp1 + temp2).template cast<T>();
  }
};

// sqrt(x) = x^(1/2)
template <typename T>
struct SqrtFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    out.device(d) = x.sqrt();
  }
};

template <typename T>
struct SqrtGradFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    dx.device(d) = static_cast<T>(0.5) * dout / out;
  }
};

// ceil(x) = ceiling(x)
template <typename T>
struct CeilFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    out.device(d) = x.ceil();
  }
};

template <typename T>
struct ZeroGradFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    dx.device(d) = static_cast<T>(0) / out;
  }
};

// floor(x) = flooring(x)
template <typename T>
struct FloorFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    out.device(d) = x.floor();
  }
};

template <typename T>
struct Sine {
  HOSTDEVICE T operator()(const T& val) const { return sin(val); }
};

template <>
struct Sine<platform::float16> {
  HOSTDEVICE platform::float16 operator()(const platform::float16& val) const {
    return platform::float16(sin(static_cast<float>(val)));
  }
};

template <typename T>
struct Cosine {
  HOSTDEVICE T operator()(const T& val) const { return cos(val); }
};

template <>
struct Cosine<platform::float16> {
  HOSTDEVICE platform::float16 operator()(const platform::float16& val) const {
    return platform::float16(cos(static_cast<float>(val)));
  }
};

// cosine'(x) = -sin(x)
template <typename T>
struct CosGradFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    dx.device(d) = -dout * x.unaryExpr(Sine<T>());
  }
};

// cosine(x) = cos(x)
template <typename T>
struct CosFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    out.device(d) = x.unaryExpr(Cosine<T>());
  }
};

// sine'(x) = cos(x)
template <typename T>
struct SinGradFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    dx.device(d) = dout * x.unaryExpr(Cosine<T>());
  }
};

// sine(x) = sin(x)
template <typename T>
struct SinFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    out.device(d) = x.unaryExpr(Sine<T>());
  }
};

template <typename T>
struct Acos {
  HOSTDEVICE T operator()(const T& val) const { return acos(val); }
};

template <>
struct Acos<platform::float16> {
  HOSTDEVICE platform::float16 operator()(const platform::float16& val) const {
    return platform::float16(acos(static_cast<float>(val)));
  }
};

// Acos(x) = acos(x)
template <typename T>
struct AcosFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    out.device(d) = x.unaryExpr(Acos<T>());
  }
};

// acos'(x) = -1/sqrt(1-x^2)
template <typename T>
struct AcosGradFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    dx.device(d) =
        -dout * static_cast<T>(1) / (static_cast<T>(1) - x.square()).sqrt();
  }
};

template <typename T>
struct Asin {
  HOSTDEVICE T operator()(const T& val) const { return asin(val); }
};

template <>
struct Asin<platform::float16> {
  HOSTDEVICE platform::float16 operator()(const platform::float16& val) const {
    return platform::float16(asin(static_cast<float>(val)));
  }
};

// Asin(x) = asin(x)
template <typename T>
struct AsinFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    out.device(d) = x.unaryExpr(Asin<T>());
  }
};

// asin'(x) = 1/sqrt(1-x^2)
template <typename T>
struct AsinGradFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    dx.device(d) =
        dout * static_cast<T>(1) / (static_cast<T>(1) - x.square()).sqrt();
  }
};

template <typename T>
struct Atan {
  HOSTDEVICE T operator()(const T& val) const { return atan(val); }
};

template <>
struct Atan<platform::float16> {
  HOSTDEVICE platform::float16 operator()(const platform::float16& val) const {
    return platform::float16(atan(static_cast<float>(val)));
  }
};

// Atan(x) = atan(x)
template <typename T>
struct AtanFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    out.device(d) = x.unaryExpr(Atan<T>());
  }
};

// atan'(x) =  1 / (1 + x^2)
template <typename T>
struct AtanGradFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    dx.device(d) = dout * static_cast<T>(1) / (static_cast<T>(1) + x.square());
  }
};

// round(x) = [x]
template <typename T>
struct RoundFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    out.device(d) = x.round();
  }
};

// abs(x) = |x|
template <typename T>
struct AbsFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    out.device(d) = x.abs();
  }
};

template <typename T>
struct AbsGradFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    dx.device(d) = dout * x.sign();
  }
};

// reciprocal(x) = 1 / x
template <typename T>
struct ReciprocalFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    out.device(d) = static_cast<T>(1) / x;
  }
};

template <typename T>
struct ReciprocalGradFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    dx.device(d) = dout * static_cast<T>(-1) * out * out;
  }
};

// log(x) = natural logarithm of x
template <typename T>
struct LogFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    out.device(d) = x.log();
  }
};

template <typename T>
struct LogGradFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    dx.device(d) = dout * (static_cast<T>(1) / x);
  }
};

// square(x) = x^2
template <typename T>
struct SquareFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    out.device(d) = x.square();
  }
};

template <typename T>
struct SquareGradFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    dx.device(d) = dout * static_cast<T>(2) * x;
  }
};

template <typename T>
struct BReluFunctor : public BaseActivationFunctor<T> {
  float t_min;
  float t_max;

  // NOTE: Explicit hides the `BaseActivationFunctor<T>::GetAttrs`
  // not polymorphism for speed.
  typename BaseActivationFunctor<T>::AttrPair GetAttrs() {
    return {{"t_min", &t_min}, {"t_max", &t_max}};
  }

  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    out.device(d) =
        x.cwiseMax(static_cast<T>(t_min)).cwiseMin(static_cast<T>(t_max));
  }
};

template <typename T>
struct BReluGradFunctor : public BaseActivationFunctor<T> {
  float t_min;
  float t_max;
  typename BaseActivationFunctor<T>::AttrPair GetAttrs() {
    return {{"t_min", &t_min}, {"t_max", &t_max}};
  }
  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    dx.device(d) = dout *
                   ((x > static_cast<T>(t_min)) * (x < static_cast<T>(t_max)))
                       .template cast<T>();
  }
};

// relu6(x) = min(max(0, x), 6)
template <typename T>
struct Relu6Functor : public BaseActivationFunctor<T> {
  float threshold;

  typename BaseActivationFunctor<T>::AttrPair GetAttrs() {
    return {{"threshold", &threshold}};
  }

  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    out.device(d) =
        x.cwiseMax(static_cast<T>(0)).cwiseMin(static_cast<T>(threshold));
  }
};

template <typename T>
struct Relu6GradFunctor : public BaseActivationFunctor<T> {
  float threshold;
  typename BaseActivationFunctor<T>::AttrPair GetAttrs() {
    return {{"threshold", &threshold}};
  }
  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    dx.device(d) =
        dout *
        ((out > static_cast<T>(0)) * (out < static_cast<T>(threshold)))
            .template cast<T>();
  }
};

// softplus(x) = log(1 + exp(x))
// When x is a very large positive number, exp(x) may explode to inf,
// Using trick below for numerical stability
// https://hips.seas.harvard.edu/blog/2013/01/09/computing-log-sum-exp/
// Then: softplus(x) = max(x, 0) + log(exp(-max(x, 0)) + exp(x - max(x, 0)))
template <typename T>
struct SoftplusFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) {
    auto temp = x.cwiseMax(static_cast<T>(0));  // temp = max(x, 0)
    out.device(d) = temp + (((-temp).exp() + (x - temp).exp()).log());
  }
};

// d(softplus(x))/dx = exp(x) / (1 + exp(x))
// For numerical stability:
// d(softplus(x))/dx = exp(x - max(x, 0)) / (exp(-max(x, 0)) +
// exp(x - max(x, 0)))
template <typename T>
struct SoftplusGradFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) {
    auto temp = x.cwiseMax(static_cast<T>(0));  // temp = max(x, 0)
    dx.device(d) =
        dout * ((x - temp).exp() / ((-temp).exp() + (x - temp).exp()));
  }
};

// softsign(x) = x / (1 + |x|)
template <typename T>
struct SoftsignFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) {
    out.device(d) = x / (static_cast<T>(1) + x.abs());
  }
};

// d(softsign(x))/dx = 1 / (1 + |x|)^2
// Taken from https://en.wikipedia.org/wiki/Activation_function
template <typename T>
struct SoftsignGradFunctor : public BaseActivationFunctor<T> {
  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) {
    dx.device(d) =
        dout * (static_cast<T>(1) / (static_cast<T>(1) + x.abs()).square());
  }
};

template <typename T>
struct SoftReluFunctor : public BaseActivationFunctor<T> {
  float threshold;
  typename BaseActivationFunctor<T>::AttrPair GetAttrs() {
    return {{"threshold", &threshold}};
  }

  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    auto tmp = static_cast<T>(threshold);
    auto temp = x.cwiseMax(-tmp).cwiseMin(tmp);
    out.device(d) = (static_cast<T>(1) + temp.exp()).log();
  }
};

template <typename T>
struct SoftReluGradFunctor : public BaseActivationFunctor<T> {
  float threshold;
  typename BaseActivationFunctor<T>::AttrPair GetAttrs() {
    return {{"threshold", &threshold}};
  }
  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    auto tmp = static_cast<T>(threshold);
    auto temp = ((out > -tmp) * (out < tmp)).template cast<T>().eval();
    dx.device(d) = dout * (static_cast<T>(1) - (-out).exp()) * temp;
  }
};

template <typename T>
struct LeakyReluFunctor : public BaseActivationFunctor<T> {
  float alpha;
  typename BaseActivationFunctor<T>::AttrPair GetAttrs() {
    return {{"alpha", &alpha}};
  }

  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    out.device(d) = x.cwiseMax(static_cast<T>(alpha) * x);
  }
};

template <typename T>
struct LeakyReluGradFunctor : public BaseActivationFunctor<T> {
  float alpha;
  typename BaseActivationFunctor<T>::AttrPair GetAttrs() {
    return {{"alpha", &alpha}};
  }
  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    auto temp1 = static_cast<T>(alpha) *
                 (x < static_cast<T>(0)).template cast<T>().eval();
    auto temp2 = (x >= static_cast<T>(0)).template cast<T>().eval();
    dx.device(d) = dout * (temp1 + temp2).template cast<T>();
  }
};

template <typename T>
struct ELUFunctor : public BaseActivationFunctor<T> {
  float alpha;
  typename BaseActivationFunctor<T>::AttrPair GetAttrs() {
    return {{"alpha", &alpha}};
  }

  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    out.device(d) = x.cwiseMax(static_cast<T>(0)) +
                    (static_cast<T>(alpha) * (x.exp() - static_cast<T>(1)))
                        .cwiseMin(static_cast<T>(0));
  }
};

template <typename T>
struct ELUGradFunctor : public BaseActivationFunctor<T> {
  float alpha;
  typename BaseActivationFunctor<T>::AttrPair GetAttrs() {
    return {{"alpha", &alpha}};
  }
  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    dx.device(d) = dout * (x > static_cast<T>(0)).template cast<T>() +
                   dout * (out + static_cast<T>(alpha)) *
                       (x < static_cast<T>(0)).template cast<T>();
  }
};

// FIXME(qijun) https://github.com/PaddlePaddle/Paddle/issues/5198
template <typename T>
struct PowFunctor : public BaseActivationFunctor<T> {
  float factor;
  typename BaseActivationFunctor<T>::AttrPair GetAttrs() {
    return {{"factor", &factor}};
  }
  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    out.device(d) = x.pow(static_cast<T>(factor));
  }
};

template <typename T>
struct PowGradFunctor : public BaseActivationFunctor<T> {
  float factor;
  typename BaseActivationFunctor<T>::AttrPair GetAttrs() {
    return {{"factor", &factor}};
  }
  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    dx.device(d) = dout * static_cast<T>(factor) *
                   x.pow(static_cast<T>(factor) - static_cast<T>(1));
  }
};

template <typename T>
struct STanhFunctor : public BaseActivationFunctor<T> {
  float scale_a;
  float scale_b;
  typename BaseActivationFunctor<T>::AttrPair GetAttrs() {
    return {{"scale_a", &scale_a}, {"scale_b", &scale_b}};
  }

  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    out.device(d) =
        static_cast<T>(scale_b) * (static_cast<T>(scale_a) * x).tanh();
  }
};

template <typename T>
struct STanhGradFunctor : public BaseActivationFunctor<T> {
  float scale_a;
  float scale_b;
  typename BaseActivationFunctor<T>::AttrPair GetAttrs() {
    return {{"scale_a", &scale_a}, {"scale_b", &scale_b}};
  }

  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    auto a = static_cast<T>(scale_a);
    auto b = static_cast<T>(scale_b);
    auto temp = (a * x).tanh() * (a * x).tanh();
    dx.device(d) = dout * a * b * (static_cast<T>(1) - temp);
  }
};

template <typename T>
struct ThresholdedReluFunctor : public BaseActivationFunctor<T> {
  float threshold;
  typename BaseActivationFunctor<T>::AttrPair GetAttrs() {
    return {{"threshold", &threshold}};
  }

  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    auto th = static_cast<T>(threshold);
    out.device(d) = (x > th).template cast<T>() * x;
  }
};

template <typename T>
struct ThresholdedReluGradFunctor : public BaseActivationFunctor<T> {
  float threshold;
  typename BaseActivationFunctor<T>::AttrPair GetAttrs() {
    return {{"threshold", &threshold}};
  }

  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    auto th = static_cast<T>(threshold);
    dx.device(d) = dout * (x > th).template cast<T>();
  }
};

template <typename T>
struct HardSigmoidFunctor : public BaseActivationFunctor<T> {
  float slope;
  float offset;
  typename BaseActivationFunctor<T>::AttrPair GetAttrs() {
    return {{"slope", &slope}, {"offset", &offset}};
  }

  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    auto temp = x * static_cast<T>(slope) + static_cast<T>(offset);
    out.device(d) =
        temp.cwiseMax(static_cast<T>(0)).cwiseMin(static_cast<T>(1));
  }
};

template <typename T>
struct HardSigmoidGradFunctor : public BaseActivationFunctor<T> {
  float slope;
  float offset;
  typename BaseActivationFunctor<T>::AttrPair GetAttrs() {
    return {{"slope", &slope}, {"offset", &offset}};
  }
  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    dx.device(d) = dout *
                   ((out > static_cast<T>(0)) * (out < static_cast<T>(1)))
                       .template cast<T>() *
                   static_cast<T>(slope);
  }
};

template <typename T>
struct SwishFunctor : public BaseActivationFunctor<T> {
  float beta;
  typename BaseActivationFunctor<T>::AttrPair GetAttrs() {
    return {{"beta", &beta}};
  }

  template <typename Device, typename X, typename Out>
  void operator()(Device d, X x, Out out) const {
    out.device(d) = x / (static_cast<T>(1) + (static_cast<T>(-beta) * x).exp());
  }
};

template <typename T>
struct SwishGradFunctor : public BaseActivationFunctor<T> {
  float beta;
  typename BaseActivationFunctor<T>::AttrPair GetAttrs() {
    return {{"beta", &beta}};
  }

  template <typename Device, typename X, typename Out, typename dOut,
            typename dX>
  void operator()(Device d, X x, Out out, dOut dout, dX dx) const {
    auto temp1 = static_cast<T>(1) /
                 (static_cast<T>(1) + (static_cast<T>(-beta) * x).exp());
    auto temp2 = temp1 * (static_cast<T>(1) - (static_cast<T>(beta) * out));
    dx.device(d) = dout * ((static_cast<T>(beta) * out) + temp2);
  }
};

}  // namespace operators
}  // namespace paddle

#define FOR_EACH_KERNEL_FUNCTOR(__macro)                             \
  __macro(sigmoid, SigmoidFunctor, SigmoidGradFunctor);              \
  __macro(logsigmoid, LogSigmoidFunctor, LogSigmoidGradFunctor);     \
  __macro(exp, ExpFunctor, ExpGradFunctor);                          \
  __macro(relu, ReluFunctor, ReluGradFunctor);                       \
  __macro(gelu, GeluFunctor, GeluGradFunctor);                       \
  __macro(tanh, TanhFunctor, TanhGradFunctor);                       \
  __macro(atan, AtanFunctor, AtanGradFunctor);                       \
  __macro(softshrink, SoftShrinkFunctor, SoftShrinkGradFunctor);     \
  __macro(sqrt, SqrtFunctor, SqrtGradFunctor);                       \
  __macro(abs, AbsFunctor, AbsGradFunctor);                          \
  __macro(ceil, CeilFunctor, ZeroGradFunctor);                       \
  __macro(floor, FloorFunctor, ZeroGradFunctor);                     \
  __macro(cos, CosFunctor, CosGradFunctor);                          \
  __macro(acos, AcosFunctor, AcosGradFunctor);                       \
  __macro(sin, SinFunctor, SinGradFunctor);                          \
  __macro(asin, AsinFunctor, AsinGradFunctor);                       \
  __macro(round, RoundFunctor, ZeroGradFunctor);                     \
  __macro(reciprocal, ReciprocalFunctor, ReciprocalGradFunctor);     \
  __macro(log, LogFunctor, LogGradFunctor);                          \
  __macro(square, SquareFunctor, SquareGradFunctor);                 \
  __macro(brelu, BReluFunctor, BReluGradFunctor);                    \
  __macro(soft_relu, SoftReluFunctor, SoftReluGradFunctor);          \
  __macro(pow, PowFunctor, PowGradFunctor);                          \
  __macro(stanh, STanhFunctor, STanhGradFunctor);                    \
  __macro(softplus, SoftplusFunctor, SoftplusGradFunctor);           \
  __macro(softsign, SoftsignFunctor, SoftsignGradFunctor);           \
  __macro(relu6, Relu6Functor, Relu6GradFunctor);                    \
  __macro(leaky_relu, LeakyReluFunctor, LeakyReluGradFunctor);       \
  __macro(tanh_shrink, TanhShrinkFunctor, TanhShrinkGradFunctor);    \
  __macro(elu, ELUFunctor, ELUGradFunctor);                          \
  __macro(hard_shrink, HardShrinkFunctor, HardShrinkGradFunctor);    \
  __macro(hard_sigmoid, HardSigmoidFunctor, HardSigmoidGradFunctor); \
  __macro(swish, SwishFunctor, SwishGradFunctor);                    \
  __macro(thresholded_relu, ThresholdedReluFunctor, ThresholdedReluGradFunctor);
