/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/core/framework/function.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/tensor_util.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/threadpool.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/core/util/batch_util.h"
#include "tensorflow/core/util/reffed_status_callback.h"

namespace tensorflow {
namespace data {
namespace {

void SetRunOptions(OpKernelContext* ctx, FunctionLibraryRuntime::Options* opts,
                   bool always_collect_stats) {
  opts->step_id = ctx->step_id();
  opts->rendezvous = ctx->rendezvous();
  if (always_collect_stats) {
    opts->stats_collector = ctx->stats_collector();
  }
  opts->runner = ctx->runner();
}

class MapDefunOp : public AsyncOpKernel {
 public:
  explicit MapDefunOp(OpKernelConstruction* ctx) : AsyncOpKernel(ctx) {
    auto func_lib = ctx->function_library();
    OP_REQUIRES(ctx, func_lib != nullptr,
                errors::Internal("No function library."));
    const NameAttrList* func;
    OP_REQUIRES_OK(ctx, ctx->GetAttr("f", &func));
    OP_REQUIRES_OK(ctx,
                   func_lib->Instantiate(func->name(), AttrSlice(&func->attr()),
                                         &func_handle_));
    OP_REQUIRES_OK(ctx, ctx->GetAttr("output_shapes", &output_shapes_));

    OP_REQUIRES(ctx, ctx->num_inputs() >= 0,
                errors::InvalidArgument("Must have at least one input."));
    OP_REQUIRES(ctx, ctx->num_outputs() >= 0,
                errors::InvalidArgument("Must have at least one output."));
    OP_REQUIRES(ctx, ctx->num_outputs() == output_shapes_.size(),
                errors::InvalidArgument(
                    "Length of output_shapes and output_types must match."));
  }

  ~MapDefunOp() override {}

  void ComputeAsync(OpKernelContext* ctx, DoneCallback done) override {
    ComputeOptions* compute_opts = nullptr;

    OP_REQUIRES_OK_ASYNC(ctx, SetupArgs(ctx, &compute_opts), done);

    Status s = SetupOutputs(ctx, compute_opts);
    if (!s.ok()) delete compute_opts;
    OP_REQUIRES_OK_ASYNC(ctx, s, done);

    FunctionLibraryRuntime::Options opts;
    SetRunOptions(ctx, &opts, false);

    // Run loop
    StatusCallback callback = std::bind(
        [](OpKernelContext* ctx, ComputeOptions* compute_opts,
           DoneCallback& done, const Status& status) {
          delete compute_opts;
          ctx->SetStatus(status);
          done();
        },
        ctx, compute_opts, std::move(done), std::placeholders::_1);

    auto* refcounted = new ReffedStatusCallback(std::move(callback));

    CancellationManager* parent_mgr = ctx->cancellation_manager();

    for (size_t i = 0; i < static_cast<size_t>(compute_opts->batch_size); ++i) {
      // We use a different cancellation manager each time the function is run
      // to avoid the race condition between a function run error and other
      // functions being cancelled as a result.
      CancellationManager* c_mgr = new CancellationManager();
      CancellationToken token = parent_mgr->get_cancellation_token();
      const bool success = parent_mgr->RegisterCallback(
          token, [c_mgr]() { c_mgr->StartCancel(); });

      opts.cancellation_manager = c_mgr;
      if (!success) {
        delete c_mgr;
        refcounted->UpdateStatus(errors::Cancelled(
            "MapDefunOp functions cancelled because parent graph cancelled"));
        break;
      }

      auto* call_frame = new MapFunctionCallFrame(compute_opts, this, i);

      refcounted->Ref();
      ctx->function_library()->Run(opts, func_handle_, call_frame,
                                   [call_frame, refcounted, c_mgr, parent_mgr,
                                    token](const Status& func_status) {
                                     parent_mgr->DeregisterCallback(token);
                                     delete c_mgr;
                                     delete call_frame;
                                     refcounted->UpdateStatus(func_status);
                                     refcounted->Unref();
                                   });
    }

    // Unref 1 because refcounted is initialized with refcount = 1
    refcounted->Unref();
  }

 private:
  FunctionLibraryRuntime::Handle func_handle_;
  std::vector<PartialTensorShape> output_shapes_;

  struct ComputeOptions {
    // These vary per MapDefunOp::ComputeAsync call, but must persist until
    // all calls to the function are complete. This struct also encapsulates
    // all the components that need to be passed to each MapFunctionCallFrame.

    OpInputList args;
    const std::vector<TensorShape> arg_shapes;
    OpInputList captured_inputs;
    const int64 batch_size;

    // Output of a compute call
    std::vector<PartialTensorShape> output_shapes GUARDED_BY(mu);
    OpOutputList output GUARDED_BY(mu);
    mutex mu;

    // Create a copy of output_shapes because every `Compute` may expect a
    // different output shape.
    ComputeOptions(OpInputList args, OpInputList captured_inputs,
                   std::vector<TensorShape> arg_shapes, int64 batch_size,
                   const std::vector<PartialTensorShape>& output_shapes_attr)
        : args(args),
          arg_shapes(std::move(arg_shapes)),
          captured_inputs(captured_inputs),
          batch_size(batch_size),
          output_shapes(output_shapes_attr) {}
  };

  // Get inputs to Compute and check that they are valid.
  Status SetupArgs(OpKernelContext* ctx, ComputeOptions** compute_opts) {
    OpInputList arguments;
    TF_RETURN_IF_ERROR(ctx->input_list("arguments", &arguments));
    OpInputList captured_inputs;
    TF_RETURN_IF_ERROR(ctx->input_list("captured_inputs", &captured_inputs));

    int64 batch_size = arguments[0].dims() > 0 ? arguments[0].dim_size(0) : -1;

    for (size_t i = 0; i < arguments.size(); ++i) {
      if (arguments[i].dims() == 0) {
        return errors::InvalidArgument(
            "All inputs must have rank at least 1. Input ", i,
            " has a rank of 0.");
      } else if (arguments[i].dim_size(0) != batch_size) {
        return errors::InvalidArgument(
            "All inputs must have the same dimension 0. Input ", i,
            " has leading dimension ", ctx->input(i).dim_size(0),
            ", while all previous inputs have leading dimension ", batch_size);
      }
    }

    std::vector<TensorShape> arg_shapes;
    arg_shapes.reserve(arguments.size());

    for (size_t i = 0; i < arguments.size(); ++i) {
      arg_shapes.push_back(arguments[i].shape());
      arg_shapes.at(i).RemoveDim(0);
    }

    *compute_opts =
        new ComputeOptions(arguments, captured_inputs, std::move(arg_shapes),
                           batch_size, output_shapes_);
    return Status::OK();
  }

  Status SetupOutputs(OpKernelContext* ctx, ComputeOptions* opts) {
    mutex_lock l(opts->mu);
    TF_RETURN_IF_ERROR(ctx->output_list("output", &opts->output));

    for (size_t i = 0; i < output_types().size(); ++i) {
      if (output_shapes_.at(i).IsFullyDefined()) {
        Tensor* out = nullptr;
        TensorShape output_shape;
        output_shapes_.at(i).AsTensorShape(&output_shape);
        output_shape.InsertDim(0, opts->batch_size);
        TF_RETURN_IF_ERROR(opts->output.allocate(i, output_shape, &out));
      }
    }
    return Status::OK();
  }

  class MapFunctionCallFrame : public CallFrameInterface {
   public:
    MapFunctionCallFrame(ComputeOptions* compute_opts, OpKernel* kernel,
                         size_t iter)
        : compute_opts_(compute_opts), kernel_(kernel), iter_(iter) {}

    ~MapFunctionCallFrame() override {}

    size_t num_args() const override { return compute_opts_->args.size(); }

    size_t num_retvals() const override {
      return static_cast<size_t>(kernel_->num_outputs());
    }

    Status GetArg(int index, Tensor* val) const override {
      if (index < 0 || index >= compute_opts_->args.size() +
                                    compute_opts_->captured_inputs.size()) {
        return errors::InvalidArgument(
            "Mismatch in number of function inputs.");
      }

      if (index >= compute_opts_->args.size()) {
        // The function is calling for a captured input
        *val =
            compute_opts_->captured_inputs[index - compute_opts_->args.size()];
        return Status::OK();
      }

      bool result =
          val->CopyFrom(compute_opts_->args[index].Slice(iter_, iter_ + 1),
                        compute_opts_->arg_shapes.at(index));
      if (!result) {
        return errors::Internal("GetArg failed.");
      } else if (!val->IsAligned()) {
        // Ensure alignment
        *val = tensor::DeepCopy(*val);
      }
      return Status::OK();
    }

    Status SetRetval(int index, const Tensor& val) override {
      if (index < 0 || index >= kernel_->num_outputs()) {
        return errors::InvalidArgument(
            "Mismatch in number of function outputs.");
      }

      if (val.dtype() != kernel_->output_type(index)) {
        return errors::InvalidArgument(
            "Mismatch in function return type and expected output type for "
            "output: ",
            index);
      }
      {  // Locking scope
        mutex_lock l(compute_opts_->mu);
        if (!compute_opts_->output_shapes.at(index).IsCompatibleWith(
                val.shape())) {
          return errors::InvalidArgument(
              "Mismatch in function retval shape, ", val.shape(),
              ", and expected output shape, ",
              compute_opts_->output_shapes.at(index).DebugString(), ".");
        }
        if (!compute_opts_->output_shapes.at(index).IsFullyDefined()) {
          // Given val, we have new information about the output shape at
          // this index. Store the shape and allocate the output accordingly.
          compute_opts_->output_shapes.at(index) = val.shape();

          Tensor* out = nullptr;
          TensorShape actual_shape = val.shape();
          actual_shape.InsertDim(0, compute_opts_->batch_size);
          TF_RETURN_IF_ERROR(
              compute_opts_->output.allocate(index, actual_shape, &out));
        }
        return batch_util::CopyElementToSlice(
            val, (compute_opts_->output)[index], iter_);
      }
    }

   private:
    ComputeOptions* const compute_opts_;  // Not owned
    const OpKernel* kernel_;
    const size_t iter_;
  };
};

REGISTER_KERNEL_BUILDER(Name("MapDefun").Device(DEVICE_CPU), MapDefunOp);
}  // namespace
}  // namespace data
}  // namespace tensorflow
