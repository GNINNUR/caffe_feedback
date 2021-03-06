// Copyright 2014 BVLC and contributors.

#include <algorithm>
#include <cfloat>
#include <vector>

#include "caffe/layer.hpp"
#include "caffe/vision_layers.hpp"
#include "caffe/util/math_functions.hpp"

using std::max;
using std::min;

namespace caffe {

  template <typename Dtype>
  void PoolingLayer<Dtype>::SetUp(const vector<Blob<Dtype>*>& bottom,
				  vector<Blob<Dtype>*>* top) {
    CHECK_EQ(bottom.size(), 1) << "PoolingLayer takes a single blob as input.";
    CHECK_EQ(top->size(), 1) << "PoolingLayer takes a single blob as output.";
    kernel_size_ = this->layer_param_.pooling_param().kernel_size();
    stride_ = this->layer_param_.pooling_param().stride();
    pad_ = this->layer_param_.pooling_param().pad();
    if (pad_ != 0) {
      CHECK_EQ(this->layer_param_.pooling_param().pool(),
	       PoolingParameter_PoolMethod_AVE)
	<< "Padding implemented only for average pooling.";
    }
    channels_ = bottom[0]->channels();
    height_ = bottom[0]->height();
    width_ = bottom[0]->width();
    //For debug
    pooled_height_ = static_cast<int>(ceil(static_cast<float>(
							      height_ + 2 * pad_ - kernel_size_) / stride_)) + 1;
    pooled_width_ = static_cast<int>(ceil(static_cast<float>(
							     width_ + 2 * pad_ - kernel_size_) / stride_)) + 1;
    (*top)[0]->Reshape(bottom[0]->num(), channels_, pooled_height_,
		       pooled_width_);
    // If stochastic pooling, we will initialize the random index part.
    if (this->layer_param_.pooling_param().pool() ==
        PoolingParameter_PoolMethod_STOCHASTIC) {
      rand_idx_.Reshape(bottom[0]->num(), channels_, pooled_height_,
			pooled_width_);
    }
    this->pooling_mask_.Reshape(bottom[0]->num(), this->channels_,
				this->pooled_height_, this->pooled_width_);
    //Setting up the eq_filter
    this->eq_filter_ = new Blob<Dtype>(bottom[0]->num(), 1, 1, bottom[0]->channels() * bottom[0]->height() * bottom[0]->width());
  }

  // TODO(Yangqing): Is there a faster way to do pooling in the channel-first
  // case?
  template <typename Dtype>
  Dtype PoolingLayer<Dtype>::Forward_cpu(const vector<Blob<Dtype>*>& bottom,
					 vector<Blob<Dtype>*>* top) {
    const Dtype* bottom_data = bottom[0]->cpu_data();
    Dtype* top_data = (*top)[0]->mutable_cpu_data();
    // Different pooling methods. We explicitly do the switch outside the for
    // loop to save time, although this results in more codes.
    int top_count = (*top)[0]->count();
    // first, clear pooling_mask
    memset(pooling_mask_.mutable_cpu_data(), 0, pooling_mask_.count() * sizeof(Dtype));
    switch (this->layer_param_.pooling_param().pool()) {
    case PoolingParameter_PoolMethod_MAX:
      // Initialize
      for (int i = 0; i < top_count; ++i) {
	top_data[i] = -FLT_MAX;
      }
      // The main loop
      for (int n = 0; n < bottom[0]->num(); ++n) {
	for (int c = 0; c < channels_; ++c) {
	  Dtype* mask_data = pooling_mask_.mutable_cpu_data() 
	    + pooling_mask_.offset(n,c);
	  for (int ph = 0; ph < pooled_height_; ++ph) {
	    for (int pw = 0; pw < pooled_width_; ++pw) {
	      int hstart = ph * stride_;
	      int wstart = pw * stride_;
	      int hend = min(hstart + kernel_size_, height_);
	      int wend = min(wstart + kernel_size_, width_);

	      for (int h = hstart; h < hend; ++h) {
		for (int w = wstart; w < wend; ++w) {
		  if(top_data[ph * pooled_width_ + pw] < bottom_data[h * width_ + w]){
		    top_data[ph * pooled_width_ + pw] = bottom_data[h * width_ + w];
		    mask_data[ph * pooled_width_ + pw] = static_cast<Dtype> (h * width_ + w);
		  }
		}
	      }
	    }
	  }
	  // compute offset
	  bottom_data += bottom[0]->offset(0, 1);
	  top_data += (*top)[0]->offset(0, 1);
	}
      }
      break;
    case PoolingParameter_PoolMethod_AVE:
      for (int i = 0; i < top_count; ++i) {
	top_data[i] = 0;
      }
      // The main loop
      // For each image in the mini-batch
      for (int n = 0; n < bottom[0]->num(); ++n) {
	// For each channel
	for (int c = 0; c < channels_; ++c) {
	  for (int ph = 0; ph < pooled_height_; ++ph) {
	    for (int pw = 0; pw < pooled_width_; ++pw) {
	      int hstart = ph * stride_ - pad_;
	      int wstart = pw * stride_ - pad_;
	      int hend = min(hstart + kernel_size_, height_ + pad_);
	      int wend = min(wstart + kernel_size_, width_ + pad_);
	      int pool_size = (hend - hstart) * (wend - wstart);
	      hstart = max(hstart, 0);
	      wstart = max(wstart, 0);
	      hend = min(hend, height_);
	      wend = min(wend, width_);
	      for (int h = hstart; h < hend; ++h) {
		for (int w = wstart; w < wend; ++w) {
		  top_data[ph * pooled_width_ + pw] +=
		    bottom_data[h * width_ + w];
		}
	      }
	      top_data[ph * pooled_width_ + pw] /= pool_size;
	    }
	  }
	  // compute offset
	  bottom_data += bottom[0]->offset(0, 1);
	  top_data += (*top)[0]->offset(0, 1);
	}
      }
      break;
    case PoolingParameter_PoolMethod_STOCHASTIC:
      NOT_IMPLEMENTED;
      break;
    default:
      LOG(FATAL) << "Unknown pooling method.";
    }
    return Dtype(0.);
  }

  template <typename Dtype>
  void PoolingLayer<Dtype>::Backward_cpu(const vector<Blob<Dtype>*>& top,
					 const bool propagate_down, vector<Blob<Dtype>*>* bottom) {
    if (!propagate_down) {
      return;
    }
    const Dtype* top_diff = top[0]->cpu_diff();
    const Dtype* top_data = top[0]->cpu_data();
    const Dtype* bottom_data = (*bottom)[0]->cpu_data();
    Dtype* bottom_diff = (*bottom)[0]->mutable_cpu_diff();
    // Different pooling methods. We explicitly do the switch outside the for
    // loop to save time, although this results in more codes.
    memset(bottom_diff, 0, (*bottom)[0]->count() * sizeof(Dtype));
    switch (this->layer_param_.pooling_param().pool()) {
    case PoolingParameter_PoolMethod_MAX:
      // The main loop
      for (int n = 0; n < top[0]->num(); ++n) {
	for (int c = 0; c < channels_; ++c) {
	  for (int ph = 0; ph < pooled_height_; ++ph) {
	    for (int pw = 0; pw < pooled_width_; ++pw) {
	      int hstart = ph * stride_;
	      int wstart = pw * stride_;
	      int hend = min(hstart + kernel_size_, height_);
	      int wend = min(wstart + kernel_size_, width_);
	      for (int h = hstart; h < hend; ++h) {
		for (int w = wstart; w < wend; ++w) {
		  bottom_diff[h * width_ + w] +=
		    top_diff[ph * pooled_width_ + pw] *
		    (bottom_data[h * width_ + w] ==
		     top_data[ph * pooled_width_ + pw]);
		}
	      }
	    }
	  }
	  // offset
	  bottom_data += (*bottom)[0]->offset(0, 1);
	  top_data += top[0]->offset(0, 1);
	  bottom_diff += (*bottom)[0]->offset(0, 1);
	  top_diff += top[0]->offset(0, 1);
	}
      }
      break;
    case PoolingParameter_PoolMethod_AVE:
      // The main loop
      for (int n = 0; n < top[0]->num(); ++n) {
	for (int c = 0; c < channels_; ++c) {
	  for (int ph = 0; ph < pooled_height_; ++ph) {
	    for (int pw = 0; pw < pooled_width_; ++pw) {
	      int hstart = ph * stride_ - pad_;
	      int wstart = pw * stride_ - pad_;
	      int hend = min(hstart + kernel_size_, height_ + pad_);
	      int wend = min(wstart + kernel_size_, width_ + pad_);
	      int pool_size = (hend - hstart) * (wend - wstart);
	      hstart = max(hstart, 0);
	      wstart = max(wstart, 0);
	      hend = min(hend, height_);
	      wend = min(wend, width_);
	      for (int h = hstart; h < hend; ++h) {
		for (int w = wstart; w < wend; ++w) {
		  bottom_diff[h * width_ + w] +=
		    top_diff[ph * pooled_width_ + pw] / pool_size;
		}
	      }
	    }
	  }
	  // offset
	  bottom_data += (*bottom)[0]->offset(0, 1);
	  top_data += top[0]->offset(0, 1);
	  bottom_diff += (*bottom)[0]->offset(0, 1);
	  top_diff += top[0]->offset(0, 1);
	}
      }
      break;
    case PoolingParameter_PoolMethod_STOCHASTIC:
      NOT_IMPLEMENTED;
      break;
    default:
      LOG(FATAL) << "Unknown pooling method.";
    }
  }

  template <typename Dtype>
  void PoolingLayer<Dtype>::UpdateEqFilter(const Blob<Dtype>* top_filter,
					   const vector<Blob<Dtype>*>& input) {
    //LOG(INFO)<<"Calculating Feedback Weights for "<<this->layer_param_.name();

    if(this->layer_param_.pooling_param().pool() == PoolingParameter_PoolMethod_MAX){
      //dealing with MAX_POOLING
      int M_ = input[0]->num();
      // LOG(ERROR) << "M_ " << M_;
      int K_ = input[0]->count() / input[0]->num();
      // LOG(ERROR) << "K_ " << K_;
      int N_ = this->pooled_height_ * this->pooled_width_;
      // LOG(ERROR) << "N_ " << N_;
      int top_output_num = top_filter->height();
      // LOG(ERROR) << "top_filter height " << top_output_num;
      int top_output_channel = top_filter->channels();
      // LOG(ERROR) << "top_filter channel " << top_output_channel;

      //this->eq_filter_->Reshape(M_, top_output_channel, top_output_num, K_);
      const Dtype* top_filter_data = top_filter->cpu_data();
      Dtype* eq_filter_data_ = this->eq_filter_->mutable_cpu_data();
      //Calculation of eq_filter_
      //Initialize as all 0
      memset(eq_filter_data_, 0, sizeof(Dtype) * this->eq_filter_->count());

      for (int n = 0; n<input[0]->num(); n++){
	for (int c = 0; c< pooling_mask_.channels(); c++){
	  const Dtype* mask_data = pooling_mask_.cpu_data() + pooling_mask_.offset(n,c);
	  for (int offset = 0; offset < pooling_mask_.height() * pooling_mask_.width(); ++offset) {
	    int mask_offset = static_cast<int>(*(mask_data + offset));
      for(int top_c = 0; top_c < top_filter->channels(); top_c++){
	      for(int top_o = 0; top_o< top_output_num; top_o++) {
		Dtype _f_value = *(top_filter_data + top_filter->offset(n, top_c, top_o) +
				   c * this->pooled_height_ * this->pooled_width_ + offset);
		//NOTICE:		
		//a neuron could be activated multiple times in max pooling
		//because the MAX_POOLING is overlapped
		*(eq_filter_data_ + this->eq_filter_->offset(n, top_c, top_o) +
		  c * this->height_ * this->width_ + mask_offset) += _f_value;
	      }
	    }
	  }
	}//for each channel
      }	//for each image in mini-batch
    }
    else if(this->layer_param_.pooling_param().pool() == PoolingParameter_PoolMethod_AVE){
      //dealing with AVE_POOLING
      //ToDo: add implementation of average pooling     
    }
  }

  INSTANTIATE_CLASS(PoolingLayer);


}  // namespace caffe
