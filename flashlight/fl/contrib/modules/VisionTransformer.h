/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "flashlight/fl/nn/modules/Container.h"
#include "flashlight/fl/nn/modules/LayerNorm.h"
#include "flashlight/fl/nn/modules/Linear.h"
#include "flashlight/fl/nn/modules/Module.h"

namespace fl {

class VisionTransformer : public Container {
 public:
  VisionTransformer(const std::string& prefix, float pLayerdrop);
  VisionTransformer(
      int32_t modelDim,
      int32_t headDim,
      int32_t mlpDim,
      int32_t nHeads,
      float pDropout,
      float pLayerdrop);

  std::vector<Variable> forward(const std::vector<Variable>& input) override;
  std::string prettyString() const override;

  static fl::Variable initLinear(int32_t inDim, int32_t outDim);

 private:
  int32_t modelDim_;
  int32_t headDim_;
  int32_t mlpDim_;
  int32_t nHeads_;
  double pDropout_;
  double pLayerdrop_;
  std::shared_ptr<Linear> w1_, w2_;
  std::shared_ptr<Linear> wq_, wk_, wv_;
  //   std::shared_ptr<Linear> wqkv_;
  std::shared_ptr<Linear> wf_;
  std::shared_ptr<LayerNorm> norm1_, norm2_;

  Variable gelu(const Variable& input);
  Variable mlp(const Variable& input);
  Variable selfAttention(const Variable& input);
  Variable dropPath(const Variable& input);

  FL_SAVE_LOAD_WITH_BASE(
      Container,
      w1_,
      w2_,
      wq_,
      wk_,
      wv_,
      //   wqkv_,
      wf_,
      norm1_,
      norm2_,
      modelDim_,
      headDim_,
      mlpDim_,
      nHeads_,
      pDropout_,
      pLayerdrop_)

  VisionTransformer();
};

} // namespace fl

CEREAL_REGISTER_TYPE(fl::VisionTransformer);
