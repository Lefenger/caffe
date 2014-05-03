// Copyright 2014 BVLC and contributors.

#ifndef CAFFE_TEST_GRADIENT_CHECK_UTIL_H_
#define CAFFE_TEST_GRADIENT_CHECK_UTIL_H_

#include <glog/logging.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "caffe/filler.hpp"
#include "caffe/layer.hpp"
#include "caffe/net.hpp"

using std::min;
using std::max;

namespace caffe {

// The gradient checker adds a L2 normalization loss function on top of the
// top blobs, and checks the gradient.
template <typename Dtype>
class GradientChecker {
 public:
  // kink and kink_range specify an ignored nonsmooth region of the form
  // kink - kink_range <= |feature value| <= kink + kink_range,
  // which accounts for all nonsmoothness in use by caffe
  GradientChecker(const Dtype stepsize, const Dtype threshold,
      const unsigned int seed = 1701, const Dtype kink = 0.,
      const Dtype kink_range = -1)
      : stepsize_(stepsize), threshold_(threshold), seed_(seed),
        kink_(kink), kink_range_(kink_range) {}
  // Checks the gradient of a layer, with provided bottom layers and top
  // layers.
  // Note that after the gradient check, we do not guarantee that the data
  // stored in the layer parameters and the blobs are unchanged.
  void CheckGradient(Layer<Dtype>* layer, vector<Blob<Dtype>*>* bottom,
      vector<Blob<Dtype>*>* top, int check_bottom = -1) {
      layer->SetUp(*bottom, top);
      CheckGradientSingle(layer, bottom, top, check_bottom, -1, -1);
  }
  void CheckGradientExhaustive(Layer<Dtype>* layer,
      vector<Blob<Dtype>*>* bottom, vector<Blob<Dtype>*>* top,
      int check_bottom = -1);

  void CheckGradientSingle(Layer<Dtype>* layer, vector<Blob<Dtype>*>* bottom,
      vector<Blob<Dtype>*>* top, int check_bottom, int top_id, int top_data_id);

  // Run the forward pass with in-place computation; check that the result is
  // the same as the non-in-place result (which must have already been computed
  // in top).
  void CheckForwardInPlace(Layer<Dtype>* layer, vector<Blob<Dtype>*>* bottom,
      vector<Blob<Dtype>*>* top, int check_bottom, Dtype computed_objective);

  // Run the backward pass with in-place computation; check that the result is
  // the same as the non-in-place result (which must have already been computed
  // in bottom).
  void CheckBackwardInPlace(Layer<Dtype>* layer, vector<Blob<Dtype>*>* bottom,
      vector<Blob<Dtype>*>* top, const vector<Blob<Dtype>*>& computed_gradients,
      const vector<bool>& propagate_down,
      int check_bottom, int top_id, int top_data_id);

  // Checks the gradient of a network. This network should not have any data
  // layers or loss layers, since the function does not explicitly deal with
  // such cases yet. All input blobs and parameter blobs are going to be
  // checked, layer-by-layer to avoid numerical problems to accumulate.
  void CheckGradientNet(const Net<Dtype>& net,
      const vector<Blob<Dtype>*>& input);

 protected:
  Dtype GetObjAndGradient(vector<Blob<Dtype>*>* top, int top_id = -1,
      int top_data_id = -1);
  Dtype stepsize_;
  Dtype threshold_;
  unsigned int seed_;
  Dtype kink_;
  Dtype kink_range_;
};


template <typename Dtype>
void GradientChecker<Dtype>::CheckForwardInPlace(Layer<Dtype>* layer,
    vector<Blob<Dtype>*>* bottom, vector<Blob<Dtype>*>* top,
    int check_bottom, Dtype computed_objective) {
  vector<shared_ptr<Blob<Dtype> > > backup_bottom(bottom->size());
  vector<Blob<Dtype>*> backup_top(top->size());
  vector<bool> need_restore(min(bottom->size(), top->size()), false);
  bool check_forward_in_place = false;
  for (int i = 0; i < min(bottom->size(), top->size()); ++i) {
    if ((check_bottom < 0 || i == check_bottom) &&
        ((*top)[i]->count() == (*bottom)[i]->count()) &&
        !layer->ForwardReusesBottomData(i)) {
      backup_bottom[i].reset(new Blob<Dtype>());
      const bool copy_diff = false;
      const bool reshape = true;
      backup_bottom[i]->CopyFrom(*(*bottom)[i], copy_diff, reshape);
      backup_top[i] = (*top)[i];
      (*top)[i] = (*bottom)[i];
      need_restore[i] = true;
      check_forward_in_place = true;
    }
  }
  if (check_forward_in_place) {
    Caffe::set_random_seed(seed_);
    Dtype in_place_objective = layer->Forward(*bottom, top);
    EXPECT_EQ(computed_objective, in_place_objective);
    for (int i = 0; i < min(bottom->size(), top->size()); ++i) {
      if (need_restore[i]) {
        const Dtype* orig_top_data = backup_top[i]->cpu_data();
        const Dtype* in_place_top_data = (*top)[i]->cpu_data();
        for (int j = 0; j < (*top)[i]->count(); ++j) {
          EXPECT_EQ(orig_top_data[j], in_place_top_data[j]);
        }
        (*top)[i] = backup_top[i];
        (*bottom)[i]->CopyFrom(*backup_bottom[i]);
      }
    }
  }
}


template <typename Dtype>
void GradientChecker<Dtype>::CheckBackwardInPlace(Layer<Dtype>* layer,
    vector<Blob<Dtype>*>* bottom, vector<Blob<Dtype>*>* top,
    const vector<Blob<Dtype>*>& computed_gradients,
    const vector<bool>& propagate_down,
    int check_bottom, int top_id, int top_data_id) {
  vector<bool> backward_in_place(min(bottom->size(), top->size()), false);
  bool check_backward_in_place = false;
  for (int i = 0; i < min(bottom->size(), top->size()); ++i) {
    if ((check_bottom < 0 || i == check_bottom) &&
        ((*top)[i]->count() == (*bottom)[i]->count()) &&
        !layer->BackwardReusesTopDiff(i)) {
      backward_in_place[i] = true;
      check_backward_in_place = true;
    }
  }
  if (check_backward_in_place) {
    vector<shared_ptr<Blob<Dtype> > > temp_bottom(bottom->size());
    vector<shared_ptr<Blob<Dtype> > > temp_top(top->size());
    vector<Blob<Dtype>*> temp_pointers_bottom(bottom->size());
    vector<Blob<Dtype>*> temp_pointers_top(top->size());
    const bool copy_diff = false;
    const bool reshape = true;
    for (int i = 0; i < bottom->size(); ++i) {
      temp_bottom[i].reset(new Blob<Dtype>());
      temp_bottom[i]->CopyFrom(*(*bottom)[i], copy_diff, reshape);
      temp_pointers_bottom[i] = temp_bottom[i].get();
    }
    for (int i = 0; i < top->size(); ++i) {
      temp_top[i].reset(new Blob<Dtype>());
      temp_pointers_top[i] = temp_top[i].get();
    }
    Caffe::set_random_seed(seed_);
    layer->SetUp(temp_pointers_bottom, &temp_pointers_top);
    for (int i = 0; i < min(bottom->size(), top->size()); ++i) {
      if (backward_in_place[i]) {
        temp_bottom[i]->ShareDiff(*temp_top[i]);
      }
    }
    layer->Forward(temp_pointers_bottom, &temp_pointers_top);
    GetObjAndGradient(&temp_pointers_top, top_id, top_data_id);
    layer->Backward(temp_pointers_top, propagate_down, &temp_pointers_bottom);
    for (int i = 0; i < min(bottom->size(), top->size()); ++i) {
      const Dtype* orig_bottom_diff = computed_gradients[i]->cpu_data();
      const Dtype* in_place_bottom_diff = temp_bottom[i]->cpu_diff();
      for (int j = 0; j < (*bottom)[i]->count(); ++j) {
        EXPECT_NEAR(orig_bottom_diff[j], in_place_bottom_diff[j], threshold_);
      }
    }
  }
}


template <typename Dtype>
void GradientChecker<Dtype>::CheckGradientSingle(Layer<Dtype>* layer,
    vector<Blob<Dtype>*>* bottom, vector<Blob<Dtype>*>* top,
    int check_bottom, int top_id, int top_data_id) {
  if (layer->ElementwiseOnlyComputation() && top_id >= 0 && top_data_id >= 0) {
    CHECK_EQ(0, layer->blobs().size());
    const int top_count = (*top)[top_id]->count();
    for (int blob_id = 0; blob_id < bottom->size(); ++blob_id) {
      CHECK_EQ(top_count, (*bottom)[blob_id]->count());
    }
  }
  // First, figure out what blobs we need to check against.
  vector<Blob<Dtype>*> blobs_to_check;
  vector<int> blobs_to_check_bottom_inds;
  vector<bool> add_noise;
  vector<bool> propagate_down(bottom->size(), false);
  for (int i = 0; i < layer->blobs().size(); ++i) {
    blobs_to_check.push_back(layer->blobs()[i].get());
    add_noise.push_back(false);
    blobs_to_check_bottom_inds.push_back(-1);
  }
  if (check_bottom < 0) {
    for (int i = 0; i < bottom->size(); ++i) {
      blobs_to_check.push_back((*bottom)[i]);
      add_noise.push_back(true);
      propagate_down[i] = true;
      blobs_to_check_bottom_inds.push_back(i);
    }
  } else {
    CHECK_LT(check_bottom, bottom->size());
    blobs_to_check.push_back((*bottom)[check_bottom]);
    add_noise.push_back(true);
    propagate_down[check_bottom] = true;
    blobs_to_check_bottom_inds.push_back(check_bottom);
  }
  // Add randomly generated noise to the diff of each of the bottom
  // blobs_to_check.  We will subtract this noise off after the gradient is
  // computed.  This ensures that the layer's AccumBackward increments the diff
  // blob by its gradient, rather than just overwriting it.
  Caffe::set_random_seed(seed_);
  FillerParameter noise_filler_param;
  noise_filler_param.set_mean(10);
  noise_filler_param.set_std(1);
  GaussianFiller<Dtype> noise_filler(noise_filler_param);
  vector<shared_ptr<Blob<Dtype> > > noise_blobs(blobs_to_check.size());
  for (int blob_id = 0; blob_id < blobs_to_check.size(); ++blob_id) {
    if (add_noise[blob_id]) {
     Blob<Dtype>* current_blob = blobs_to_check[blob_id];
     noise_blobs[blob_id].reset(new Blob<Dtype>());
     noise_blobs[blob_id]->ReshapeLike(*current_blob);
     noise_filler.Fill(noise_blobs[blob_id].get());
     const int count = current_blob->count();
     const Dtype* noise = noise_blobs[blob_id]->cpu_data();
     Dtype* current_diff = current_blob->mutable_cpu_diff();
     caffe_copy(count, noise, current_diff);
    }
  }
  // Compute the gradient analytically using Backward
  Caffe::set_random_seed(seed_);
  // Get any loss from the layer
  Dtype computed_objective = layer->Forward(*bottom, top);
  // If the layer claims not to reuse its bottom data in forward, verify this
  // by doing in-place computation and checking that we get the same result.
  CheckForwardInPlace(layer, bottom, top, check_bottom, computed_objective);
  // Get additional loss from the objective
  computed_objective += GetObjAndGradient(top, top_id, top_data_id);
  // If the layer claims not to use its bottom and/or top data to compute its
  // gradient, verify this by corrupting them before running Backward.
  FillerParameter filler_param;
  filler_param.set_min(-10);
  filler_param.set_max(10);
  UniformFiller<Dtype> filler(filler_param);
  vector<shared_ptr<Blob<Dtype> > > backup_bottom(bottom->size());
  for (int i = 0; i < bottom->size(); ++i) {
    if (!layer->BackwardUsesBottomData(i)) {
      // Save a copy of original bottom data before corrupting so that we
      // can restore it before finite differencing.
      backup_bottom[i].reset(new Blob<Dtype>);
      const bool copy_diff = false;
      const bool reshape = true;
      backup_bottom[i]->CopyFrom(*(*bottom)[i], copy_diff, reshape);
      filler.Fill((*bottom)[i]);
    }
  }
  for (int i = 0; i < top->size(); ++i) {
    if (!layer->BackwardUsesTopData(i)) {
      filler.Fill((*top)[i]);
    }
  }
  vector<bool> accum_down(bottom->size(), true);
  layer->AccumBackward(*top, propagate_down, accum_down, bottom);
  // Store computed gradients for all checked blobs, subtracting the noise.
  vector<shared_ptr<Blob<Dtype> > >
      computed_gradient_blobs(blobs_to_check.size());
  vector<Blob<Dtype>*> bottom_gradient_blobs(bottom->size());
  for (int blob_id = 0; blob_id < blobs_to_check.size(); ++blob_id) {
    Blob<Dtype>* current_blob = blobs_to_check[blob_id];
    computed_gradient_blobs[blob_id].reset(new Blob<Dtype>);
    computed_gradient_blobs[blob_id]->ReshapeLike(*current_blob);
    const int count = blobs_to_check[blob_id]->count();
    const Dtype* diff = blobs_to_check[blob_id]->cpu_diff();
    Dtype* computed_gradients =
        computed_gradient_blobs[blob_id]->mutable_cpu_data();
    if (add_noise[blob_id]) {
      const Dtype* noise = noise_blobs[blob_id]->cpu_data();
      caffe_sub(count, diff, noise, computed_gradients);
    } else {
      caffe_copy(count, diff, computed_gradients);
    }
    const int bottom_id = blobs_to_check_bottom_inds[blob_id];
    if (bottom_id >= 0) {
      bottom_gradient_blobs[bottom_id] = computed_gradient_blobs[blob_id].get();
    }
  }
  // Restore original bottom data for finite differencing if we corrupted it.
  for (int i = 0; i < bottom->size(); ++i) {
    if (!layer->BackwardUsesBottomData(i)) {
      (*bottom)[i]->CopyFrom(*backup_bottom[i]);
    }
  }
  // If the layer claims not to reuse its top diff in backward, verify this
  // by doing in-place computation and checking that we get the same result.
  CheckBackwardInPlace(layer, bottom, top, bottom_gradient_blobs,
                       propagate_down, check_bottom, top_id, top_data_id);
  // Compute derivative of top w.r.t. each bottom and parameter input using
  // finite differencing.
  // LOG(ERROR) << "Checking " << blobs_to_check.size() << " blobs.";
  for (int blob_id = 0; blob_id < blobs_to_check.size(); ++blob_id) {
    Blob<Dtype>* current_blob = blobs_to_check[blob_id];
    const Dtype* computed_gradients =
        computed_gradient_blobs[blob_id]->cpu_data();
    // LOG(ERROR) << "Blob " << blob_id << ": checking "
    //     << current_blob->count() << " parameters.";
    for (int feat_id = 0; feat_id < current_blob->count(); ++feat_id) {
      // For an element-wise layer, we only need to do finite differencing to
      // compute the derivative of (*top)[top_id][top_data_id] w.r.t.
      // (*bottom)[blob_id][i] only for i == top_data_id.  For any other
      // i != top_data_id, we know the derivative is 0 by definition, and simply
      // check that that's true.
      Dtype estimated_gradient = 0;
      if (!layer->ElementwiseOnlyComputation() || (top_data_id == feat_id)
                                               || (top_data_id == -1)) {
        // Do finite differencing.
        // Compute loss with stepsize_ added to input.
        current_blob->mutable_cpu_data()[feat_id] += stepsize_;
        Caffe::set_random_seed(seed_);
        Dtype positive_objective = layer->Forward(*bottom, top);
        positive_objective += GetObjAndGradient(top, top_id, top_data_id);
        // Compute loss with stepsize_ subtracted from input.
        current_blob->mutable_cpu_data()[feat_id] -= stepsize_ * 2;
        Caffe::set_random_seed(seed_);
        Dtype negative_objective = layer->Forward(*bottom, top);
        negative_objective += GetObjAndGradient(top, top_id, top_data_id);
        // Recover original input value.
        current_blob->mutable_cpu_data()[feat_id] += stepsize_;
        estimated_gradient = (positive_objective - negative_objective) /
            stepsize_ / 2.;
      }
      Dtype computed_gradient = computed_gradients[feat_id];
      Dtype feature = current_blob->cpu_data()[feat_id];
      // LOG(ERROR) << "debug: " << current_blob->cpu_data()[feat_id] << " "
      //     << current_blob->cpu_diff()[feat_id];
      if (kink_ - kink_range_ > fabs(feature)
          || fabs(feature) > kink_ + kink_range_) {
        // We check relative accuracy, but for too small values, we threshold
        // the scale factor by 1.
        Dtype scale = max(
            max(fabs(computed_gradient), fabs(estimated_gradient)), 1.);
        EXPECT_NEAR(computed_gradient, estimated_gradient, threshold_ * scale)
          << "debug: (top_id, top_data_id, blob_id, feat_id)="
          << top_id << "," << top_data_id << "," << blob_id << "," << feat_id;
      }
      // LOG(ERROR) << "Feature: " << current_blob->cpu_data()[feat_id];
      // LOG(ERROR) << "computed gradient: " << computed_gradient
      //    << " estimated_gradient: " << estimated_gradient;
    }
  }
}

template <typename Dtype>
void GradientChecker<Dtype>::CheckGradientExhaustive(Layer<Dtype>* layer,
    vector<Blob<Dtype>*>* bottom, vector<Blob<Dtype>*>* top, int check_bottom) {
  layer->SetUp(*bottom, top);
  CHECK_GT(top->size(), 0) << "Exhaustive mode requires at least one top blob.";
  // LOG(ERROR) << "Exhaustive Mode.";
  for (int i = 0; i < top->size(); ++i) {
    // LOG(ERROR) << "Exhaustive: blob " << i << " size " << top[i]->count();
    for (int j = 0; j < (*top)[i]->count(); ++j) {
      // LOG(ERROR) << "Exhaustive: blob " << i << " data " << j;
      CheckGradientSingle(layer, bottom, top, check_bottom, i, j);
    }
  }
}

template <typename Dtype>
void GradientChecker<Dtype>::CheckGradientNet(
    const Net<Dtype>& net, const vector<Blob<Dtype>*>& input) {
  const vector<shared_ptr<Layer<Dtype> > >& layers = net.layers();
  vector<vector<Blob<Dtype>*> >& bottom_vecs = net.bottom_vecs();
  vector<vector<Blob<Dtype>*> >& top_vecs = net.top_vecs();
  for (int i = 0; i < layers.size(); ++i) {
    net.Forward(input);
    LOG(ERROR) << "Checking gradient for " << layers[i]->layer_param().name();
    CheckGradientExhaustive(*(layers[i].get()), bottom_vecs[i], top_vecs[i]);
  }
}

template <typename Dtype>
Dtype GradientChecker<Dtype>::GetObjAndGradient(vector<Blob<Dtype>*>* top,
    int top_id, int top_data_id) {
  Dtype loss = 0;
  if (top_id < 0) {
    // the loss will be half of the sum of squares of all outputs
    for (int i = 0; i < top->size(); ++i) {
      Blob<Dtype>* top_blob = (*top)[i];
      const Dtype* top_blob_data = top_blob->cpu_data();
      Dtype* top_blob_diff = top_blob->mutable_cpu_diff();
      int count = top_blob->count();
      for (int j = 0; j < count; ++j) {
        loss += top_blob_data[j] * top_blob_data[j];
      }
      // set the diff: simply the data.
      memcpy(top_blob_diff, top_blob_data, sizeof(Dtype) * top_blob->count());
    }
    loss /= 2.;
  } else {
    // the loss will be the top_data_id-th element in the top_id-th blob.
    for (int i = 0; i < top->size(); ++i) {
      Blob<Dtype>* top_blob = (*top)[i];
      Dtype* top_blob_diff = top_blob->mutable_cpu_diff();
      memset(top_blob_diff, 0, sizeof(Dtype) * top_blob->count());
    }
    loss = (*top)[top_id]->cpu_data()[top_data_id];
    (*top)[top_id]->mutable_cpu_diff()[top_data_id] = 1.;
  }
  return loss;
}

}  // namespace caffe

#endif  // CAFFE_TEST_GRADIENT_CHECK_UTIL_H_
