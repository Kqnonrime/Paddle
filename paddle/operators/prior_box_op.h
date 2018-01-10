/* Copyright (c) 2016 PaddlePaddle Authors. All Rights Reserve.

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
#include "paddle/framework/op_registry.h"
#include "paddle/operators/math/math_function.h"
#include "paddle/platform/transform.h"

namespace paddle {
namespace operators {

inline void ExpandAspectRatios(const std::vector<float>& input_aspect_ratior,
                               bool flip,
                               std::vector<float>& output_aspect_ratior) {
  constexpr float eps = 1e-6;
  output_aspect_ratior.clear();
  output_aspect_ratior.push_back(1.);
  for (size_t i = 0; i < input_aspect_ratior.size(); ++i) {
    float ar = input_aspect_ratior[i];
    bool already_exist = false;
    for (size_t j = 0; j < output_aspect_ratior.size(); ++j) {
      if (fabs(ar - output_aspect_ratior[j]) < eps) {
        already_exist = true;
        break;
      }
    }
    if (!already_exist) {
      output_aspect_ratior.push_back(ar);
      if (flip) {
        output_aspect_ratior.push_back(1. / ar);
      }
    }
  }
}

template <typename T>
struct ClipFunctor {
  HOSTDEVICE T operator()(T in) const {
    return std::min<T>(std::max<T>(in, 0.), 1.);
  }
};

template <typename Place, typename T>
class PriorBoxOpKernel : public framework::OpKernel<T> {
 public:
  void Compute(const framework::ExecutionContext& ctx) const override {
    auto* input = ctx.Input<paddle::framework::Tensor>("Input");
    auto* image = ctx.Input<paddle::framework::Tensor>("Image");
    auto* boxes = ctx.Output<paddle::framework::Tensor>("Boxes");
    auto* vars = ctx.Output<paddle::framework::Tensor>("Variances");

    auto min_sizes = ctx.Attr<std::vector<int>>("min_sizes");
    auto max_sizes = ctx.Attr<std::vector<int>>("max_sizes");
    auto input_aspect_ratio = ctx.Attr<std::vector<float>>("aspect_ratios");
    auto variances = ctx.Attr<std::vector<float>>("variances");
    auto flip = ctx.Attr<bool>("flip");
    auto clip = ctx.Attr<bool>("clip");

    std::vector<float> aspect_ratios;
    ExpandAspectRatios(input_aspect_ratio, flip, aspect_ratios);

    auto step_w = ctx.Attr<float>("step_w");
    auto step_h = ctx.Attr<float>("step_h");
    auto offset = ctx.Attr<float>("offset");

    auto img_width = image->dims()[3];
    auto img_height = image->dims()[2];

    auto layer_width = input->dims()[3];
    auto layer_height = input->dims()[2];

    float step_width, step_height;
    if (step_w == 0 || step_h == 0) {
      step_width = static_cast<float>(img_width) / layer_width;
      step_height = static_cast<float>(img_height) / layer_height;
    } else {
      step_width = step_w;
      step_height = step_h;
    }

    int num_priors = aspect_ratios.size() * min_sizes.size();
    if (max_sizes.size() > 0) {
      num_priors += max_sizes.size();
    }

    boxes->mutable_data<T>(ctx.GetPlace());
    vars->mutable_data<T>(ctx.GetPlace());

    auto e_boxes = framework::EigenTensor<T, 4>::From(*boxes);
    for (int h = 0; h < layer_height; ++h) {
      for (int w = 0; w < layer_width; ++w) {
        float center_x = (w + offset) * step_width;
        float center_y = (h + offset) * step_height;
        float box_width, box_height;
        int idx = 0;
        for (size_t s = 0; s < min_sizes.size(); ++s) {
          int min_size = min_sizes[s];
          // first prior: aspect_ratio = 1, size = min_size
          box_width = box_height = min_size;
          // xmin
          e_boxes(h, w, idx, 0) = (center_x - box_width / 2.) / img_width;
          // ymin
          e_boxes(h, w, idx, 1) = (center_y - box_height / 2.) / img_height;
          // xmax
          e_boxes(h, w, idx, 2) = (center_x + box_width / 2.) / img_width;
          // ymax
          e_boxes(h, w, idx, 3) = (center_y + box_height / 2.) / img_height;

          idx++;
          if (max_sizes.size() > 0) {
            int max_size = max_sizes[s];
            // second prior: aspect_ratio = 1,
            // size = sqrt(min_size * max_size)
            box_width = box_height = sqrt(min_size * max_size);
            // xmin
            e_boxes(h, w, idx, 0) = (center_x - box_width / 2.) / img_width;
            // ymin
            e_boxes(h, w, idx, 1) = (center_y - box_height / 2.) / img_height;
            // xmax
            e_boxes(h, w, idx, 2) = (center_x + box_width / 2.) / img_width;
            // ymax
            e_boxes(h, w, idx, 3) = (center_y + box_height / 2.) / img_height;
            idx++;
          }

          // rest of priors
          for (size_t r = 0; r < aspect_ratios.size(); ++r) {
            float ar = aspect_ratios[r];
            if (fabs(ar - 1.) < 1e-6) {
              continue;
            }
            box_width = min_size * sqrt(ar);
            box_height = min_size / sqrt(ar);
            // xmin
            e_boxes(h, w, idx, 0) = (center_x - box_width / 2.) / img_width;
            // ymin
            e_boxes(h, w, idx, 1) = (center_y - box_height / 2.) / img_height;
            // xmax
            e_boxes(h, w, idx, 2) = (center_x + box_width / 2.) / img_width;
            // ymax
            e_boxes(h, w, idx, 3) = (center_y + box_height / 2.) / img_height;
            idx++;
          }
        }
      }
    }

    if (clip) {
      platform::Transform<platform::CPUDeviceContext> trans;
      ClipFunctor<T> clip_func;
      trans(ctx.template device_context<platform::CPUDeviceContext>(),
            boxes->data<T>(), boxes->data<T>() + boxes->numel(),
            boxes->data<T>(), clip_func);
    }

    Eigen::Tensor<T, 2, Eigen::RowMajor> var_et(1, variances.size());
    for (int i = 0; i < variances.size(); ++i) {
      var_et(0, i) = variances[i];
    }

    int box_num = layer_height * layer_width * num_priors;
    auto var_dim = vars->dims();
    vars->Resize({box_num, static_cast<int>(variances.size())});

    auto e_vars = framework::EigenMatrix<T, Eigen::RowMajor>::From(*vars);
    e_vars = var_et.broadcast(Eigen::DSizes<int, 2>(box_num, 1));

    vars->Resize(var_dim);
  }
};  // namespace operators

}  // namespace operators
}  // namespace paddle
