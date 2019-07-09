/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include "tensorflow/core/kernels/data/batch_dataset_op.h"

#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/partial_tensor_shape.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/kernels/data/name_utils.h"
#include "tensorflow/core/lib/core/blocking_counter.h"
#include "tensorflow/core/lib/gtl/cleanup.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/util/batch_util.h"

namespace tensorflow {
namespace data {

// See documentation in ../../ops/dataset_ops.cc for a high-level
// description of the following op.

/* static */ constexpr const char* const BatchDatasetOp::kDatasetType;
/* static */ constexpr const char* const BatchDatasetOp::kInputDataset;
/* static */ constexpr const char* const BatchDatasetOp::kBatchSize;
/* static */ constexpr const char* const BatchDatasetOp::kDropRemainder;
/* static */ constexpr const char* const BatchDatasetOp::kParallelCopy;
/* static */ constexpr const char* const BatchDatasetOp::kOutputTypes;
/* static */ constexpr const char* const BatchDatasetOp::kOutputShapes;

constexpr char kInputImplEmpty[] = "input_impl_empty";
constexpr char kBatchDataset[] = "BatchDataset";

class BatchDatasetOp::Dataset : public DatasetBase {
 public:
  Dataset(OpKernelContext* ctx, int64 batch_size, bool drop_remainder,
          bool parallel_copy, const DatasetBase* input, int op_version)
      : DatasetBase(DatasetContext(ctx)),
        batch_size_(batch_size),
        drop_remainder_(drop_remainder),
        parallel_copy_(parallel_copy),
        input_(input),
        op_version_(op_version) {
    input_->Ref();

    // NOTE(mrry): Currently we implement "batch up to" semantics. If
    // we could tell statically that the input dataset is infinite,
    // then we could always report `batch_size` as the 0th dimension.
    const auto& input_shapes = input_->output_shapes();
    output_shapes_.reserve(input_shapes.size());
    for (const auto& input_shape : input_shapes) {
      if (drop_remainder_) {
        output_shapes_.emplace_back(
            PartialTensorShape({batch_size_}).Concatenate(input_shape));
      } else {
        output_shapes_.emplace_back(
            PartialTensorShape({-1}).Concatenate(input_shape));
      }
    }
  }

  ~Dataset() override { input_->Unref(); }

  std::unique_ptr<IteratorBase> MakeIteratorInternal(
      const string& prefix) const override {
    name_utils::IteratorPrefixParams params;
    params.op_version = op_version_;
    return absl::make_unique<Iterator>(Iterator::Params{
        this, name_utils::IteratorPrefix(kDatasetType, prefix, params)});
  }

  const DataTypeVector& output_dtypes() const override {
    return input_->output_dtypes();
  }

  const std::vector<PartialTensorShape>& output_shapes() const override {
    return output_shapes_;
  }

  string DebugString() const override {
    name_utils::DatasetDebugStringParams params;
    params.op_version = op_version_;
    params.set_args(batch_size_);
    return name_utils::DatasetDebugString(kDatasetType, params);
  }

  int64 Cardinality() const override {
    int64 n = input_->Cardinality();
    if (n == kInfiniteCardinality || n == kUnknownCardinality) {
      return n;
    }
    return n / batch_size_ + (n % batch_size_ == 0 || drop_remainder_ ? 0 : 1);
  }

 protected:
  Status AsGraphDefInternal(SerializationContext* ctx,
                            DatasetGraphDefBuilder* b,
                            Node** output) const override {
    Node* input_graph_node = nullptr;
    TF_RETURN_IF_ERROR(b->AddInputDataset(ctx, input_, &input_graph_node));
    Node* batch_size = nullptr;
    TF_RETURN_IF_ERROR(b->AddScalar(batch_size_, &batch_size));
    Node* drop_remainder = nullptr;
    TF_RETURN_IF_ERROR(b->AddScalar(drop_remainder_, &drop_remainder));
    AttrValue parallel_copy;
    b->BuildAttrValue(parallel_copy_, &parallel_copy);
    TF_RETURN_IF_ERROR(
        b->AddDataset(this, {input_graph_node, batch_size, drop_remainder},
                      {{kParallelCopy, parallel_copy}}, output));
    return Status::OK();
  }

 private:
  class Iterator : public DatasetIterator<Dataset> {
   public:
    explicit Iterator(const Params& params)
        : DatasetIterator<Dataset>(params) {}

    Status Initialize(IteratorContext* ctx) override {
      return dataset()->input_->MakeIterator(ctx, prefix(), &input_impl_);
    }

    Status GetNextInternal(IteratorContext* ctx,
                           std::vector<Tensor>* out_tensors,
                           bool* end_of_sequence) override {
      // Each row of `batch_elements` is a tuple of tensors from the
      // input iterator.
      std::vector<std::vector<Tensor>> batch_elements;
      {
        mutex_lock l(mu_);
        if (!input_impl_) {
          *end_of_sequence = true;
          return Status::OK();
        }
        batch_elements.reserve(dataset()->batch_size_);
        *end_of_sequence = false;
        for (int i = 0; i < dataset()->batch_size_ && !*end_of_sequence; ++i) {
          std::vector<Tensor> batch_element_tuple;
          TF_RETURN_IF_ERROR(
              input_impl_->GetNext(ctx, &batch_element_tuple, end_of_sequence));
          if (!*end_of_sequence) {
            batch_elements.emplace_back(std::move(batch_element_tuple));
          } else {
            input_impl_.reset();
          }
        }
      }

      if (batch_elements.empty()) {
        DCHECK(*end_of_sequence);
        return Status::OK();
      }

      if (dataset()->drop_remainder_ &&
          batch_elements.size() < dataset()->batch_size_) {
        *end_of_sequence = true;
        return Status::OK();
      }

      // Copy the retrieved batch elements into one output tensor per tuple
      // component.
      //
      // NOTE(mrry): If the input or output sizes are statically known, we
      // could potentially read the input values in-place into their
      // respective slice locations. This would require a different GetNext()
      // overload that supports zero-copy, and might make sense in an
      // optimization pass.
      const size_t num_tuple_components = batch_elements[0].size();
      const int64 num_batch_elements = batch_elements.size();
      for (size_t component_index = 0; component_index < num_tuple_components;
           ++component_index) {
        const Tensor& first_element = batch_elements[0][component_index];
        TensorShape batch_component_shape({num_batch_elements});
        // NOTE(mrry): Copy the shape of the first element here, because
        // `first_element.shape()` will become undefined after the 0th batch
        // element is moved into the output batch.
        TensorShape first_element_shape(first_element.shape());
        batch_component_shape.AppendShape(first_element_shape);
        out_tensors->emplace_back(ctx->allocator({}), first_element.dtype(),
                                  batch_component_shape);
        if (!out_tensors->back().IsInitialized()) {
          return errors::ResourceExhausted(
              "Failed to allocate memory for the batch of component ",
              component_index);
        }
        Tensor& batch_component = out_tensors->back();
        // Build the output tuple component by copying one slice
        // from each input element in the batch.
        auto copy_element_fn = [component_index, &batch_elements,
                                &batch_component](int index) {
          TF_RETURN_IF_ERROR(batch_util::CopyElementToSlice(
              std::move(batch_elements[index][component_index]),
              &batch_component, index));
          return Status::OK();
        };
        BlockingCounter counter(num_batch_elements);
        Status status;
        mutex status_mu;
        for (size_t i = 0; i < num_batch_elements; ++i) {
          if (batch_elements[i][component_index].shape() !=
              first_element_shape) {
            return errors::InvalidArgument(
                "Cannot batch tensors with different shapes in "
                "component ",
                component_index, ". First element had shape ",
                first_element_shape.DebugString(), " and element ", i,
                " had shape ",
                batch_elements[i][component_index].shape().DebugString(), ".");
          }
          if (TF_PREDICT_FALSE(dataset()->parallel_copy_)) {
            (*ctx->runner())(
                [i, &status, &status_mu, &counter, &copy_element_fn]() {
                  Status s = copy_element_fn(i);
                  {
                    mutex_lock l(status_mu);
                    status.Update(s);
                  }
                  counter.DecrementCount();
                });
          } else {
            status.Update(copy_element_fn(i));
            counter.DecrementCount();
          }
        }
        counter.Wait();
        TF_RETURN_IF_ERROR(status);
      }
      *end_of_sequence = false;
      return Status::OK();
    }

   protected:
    std::shared_ptr<model::Node> CreateNode(
        IteratorContext* ctx, model::Node::Args args) const override {
      return model::MakeKnownRatioNode(std::move(args), dataset()->batch_size_);
    }

    Status SaveInternal(IteratorStateWriter* writer) override {
      mutex_lock l(mu_);
      if (!input_impl_) {
        TF_RETURN_IF_ERROR(writer->WriteScalar(full_name(kInputImplEmpty), ""));
      } else {
        TF_RETURN_IF_ERROR(SaveInput(writer, input_impl_));
      }
      return Status::OK();
    }

    Status RestoreInternal(IteratorContext* ctx,
                           IteratorStateReader* reader) override {
      mutex_lock l(mu_);
      if (!reader->Contains(full_name(kInputImplEmpty))) {
        TF_RETURN_IF_ERROR(RestoreInput(ctx, reader, input_impl_));
      } else {
        input_impl_.reset();
      }
      return Status::OK();
    }

   private:
    mutex mu_;
    std::unique_ptr<IteratorBase> input_impl_ GUARDED_BY(mu_);
  };

  const int64 batch_size_;
  const bool drop_remainder_;
  const bool parallel_copy_;
  const DatasetBase* const input_;
  const int op_version_;
  std::vector<PartialTensorShape> output_shapes_;
};

BatchDatasetOp::BatchDatasetOp(OpKernelConstruction* ctx)
    : UnaryDatasetOpKernel(ctx),
      op_version_(ctx->def().op() == kBatchDataset ? 1 : 2) {
  if (ctx->HasAttr(kParallelCopy)) {
    OP_REQUIRES_OK(ctx, ctx->GetAttr(kParallelCopy, &parallel_copy_));
  }
}

void BatchDatasetOp::MakeDataset(OpKernelContext* ctx, DatasetBase* input,
                                 DatasetBase** output) {
  int64 batch_size = 0;
  OP_REQUIRES_OK(ctx, ParseScalarArgument<int64>(ctx, kBatchSize, &batch_size));
  OP_REQUIRES(ctx, batch_size > 0,
              errors::InvalidArgument("Batch size must be greater than zero."));

  bool drop_remainder = false;
  if (op_version_ > 1) {
    OP_REQUIRES_OK(
        ctx, ParseScalarArgument<bool>(ctx, kDropRemainder, &drop_remainder));
  }

  *output = new Dataset(ctx, batch_size, drop_remainder, parallel_copy_, input,
                        op_version_);
}

namespace {
REGISTER_KERNEL_BUILDER(Name("BatchDataset").Device(DEVICE_CPU),
                        BatchDatasetOp);

REGISTER_KERNEL_BUILDER(Name("BatchDatasetV2").Device(DEVICE_CPU),
                        BatchDatasetOp);
}  // namespace
}  // namespace data
}  // namespace tensorflow