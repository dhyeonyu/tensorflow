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

#include "tensorflow/compiler/xla/service/gpu/cudnn_convolution_algorithm_picker.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/optional.h"
#include "tensorflow/compiler/xla/literal_util.h"
#include "tensorflow/compiler/xla/service/gpu/backend_configs.pb.h"
#include "tensorflow/compiler/xla/service/gpu/buffer_comparator.h"
#include "tensorflow/compiler/xla/service/gpu/convolution_thunk.h"
#include "tensorflow/compiler/xla/service/gpu/ir_emission_utils.h"
#include "tensorflow/core/lib/strings/numbers.h"
#include "tensorflow/core/platform/mutex.h"

namespace xla {
namespace gpu {
namespace {

using absl::optional;
using se::DeviceMemoryBase;
using se::dnn::AlgorithmConfig;
using se::dnn::AlgorithmDesc;

class ScratchAllocator : public se::ScratchAllocator {
 public:
  ScratchAllocator(int device_ordinal, DeviceMemoryAllocator* memory_allocator)
      : device_ordinal_(device_ordinal), memory_allocator_(memory_allocator) {}

  int64 GetMemoryLimitInBytes(se::Stream* stream) override {
    return 1LL << 32;  // 4GB.  TODO(jlebar): Tune this?
  }
  int64 TotalAllocatedBytes() { return total_allocated_bytes_; }

  StatusOr<se::DeviceMemory<uint8>> AllocateBytes(se::Stream* stream,
                                                  int64 byte_size) override;

 private:
  const int device_ordinal_;
  DeviceMemoryAllocator* memory_allocator_;
  std::vector<OwningDeviceMemory> allocated_buffers_;
  int64 total_allocated_bytes_ = 0;
};

StatusOr<se::DeviceMemory<uint8>> ScratchAllocator::AllocateBytes(
    se::Stream* stream, int64 byte_size) {
  CHECK_GE(byte_size, 0) << "byte_size must be positive.";
  if (byte_size > GetMemoryLimitInBytes(stream)) {
    return se::port::Status(
        se::port::error::RESOURCE_EXHAUSTED,
        absl::StrFormat(
            "Allocating %d bytes exceeds the memory limit of %d bytes.",
            byte_size, GetMemoryLimitInBytes(stream)));
  }

  TF_ASSIGN_OR_RETURN(OwningDeviceMemory allocated_buffer,
                      memory_allocator_->Allocate(device_ordinal_, byte_size,
                                                  /*retry_on_failure=*/false));
  total_allocated_bytes_ += byte_size;

  se::DeviceMemoryBase buffer_addr = allocated_buffer.AsDeviceMemoryBase();
  allocated_buffers_.push_back(std::move(allocated_buffer));
  return se::DeviceMemory<uint8>(buffer_addr);
}

// Determines whether we can safely perform a winograd non-fused convolution for
// the given input and output shapes.  This works around b/68264959, an integer
// overflow in cuDNNv5 and cuDNNv6.
bool ShouldIncludeWinogradNonfusedAlgo(const Shape& input_shape,
                                       const Shape& output_shape,
                                       const ConvolutionDimensionNumbers& dnums,
                                       se::StreamExecutor* stream_exec) {
  // Skip this check for cudnn7 and newer.
  auto version = stream_exec->AsDnn()->GetVersion();
  if (version.ok() && version.ValueOrDie().major_version() >= 7) {
    return true;
  }

  int64 batch = input_shape.dimensions(dnums.input_batch_dimension());
  int64 in_depths = input_shape.dimensions(dnums.input_feature_dimension());
  int64 in_rows = input_shape.dimensions(dnums.input_spatial_dimensions(0));
  int64 in_cols =
      dnums.input_spatial_dimensions_size() == 1
          ? 1
          : input_shape.dimensions(dnums.input_spatial_dimensions(1));
  int64 out_depths = output_shape.dimensions(dnums.output_feature_dimension());

  int64 total_size = CeilOfRatio(batch, int64{16}) *
                     std::max(in_depths, out_depths) * in_cols * in_rows *
                     sizeof(float);

  const int64 threshold = 1L << 31;
  return total_size < threshold;
}

std::vector<AlgorithmDesc> GetAlgorithms(CudnnConvKind kind,
                                         bool with_winograd_nonfused,
                                         se::StreamExecutor* stream_exec) {
  std::vector<AlgorithmDesc> algorithms;
  switch (kind) {
    case CudnnConvKind::kBackwardFilter:
      CHECK(stream_exec->GetConvolveBackwardFilterAlgorithms(
          with_winograd_nonfused, &algorithms));
      break;
    case CudnnConvKind::kBackwardInput:
      CHECK(stream_exec->GetConvolveBackwardDataAlgorithms(
          with_winograd_nonfused, &algorithms));
      break;
    case CudnnConvKind::kForward:
      CHECK(stream_exec->GetConvolveAlgorithms(with_winograd_nonfused,
                                               &algorithms));
      break;
  }

  return algorithms;
}

string AlgorithmToString(const AlgorithmDesc& algo) {
  if (algo.tensor_ops_enabled()) {
    return absl::StrCat(algo.algo_id(), "+TC");
  }
  return absl::StrCat(algo.algo_id());
}

string NumBytesToString(int64 bytes) {
  return absl::StrCat(tensorflow::strings::HumanReadableNumBytes(bytes), " (",
                      bytes, "B)");
}

// Acquires a process-global lock on the device pointed to by the given
// StreamExecutor.
//
// This is used to prevent other XLA instances from trying to autotune on this
// device while we're using it.
tensorflow::mutex_lock LockGpu(const se::StreamExecutor* stream_exec) {
  static tensorflow::mutex mu(tensorflow::LINKER_INITIALIZED);
  // se::Platform*s are global singletons guaranteed to live forever.
  static auto* mutexes =
      new std::map<std::pair<const se::Platform*, /*device_ordinal*/ int64>,
                   tensorflow::mutex>();

  tensorflow::mutex_lock global_lock(mu);
  auto it = mutexes
                ->emplace(std::piecewise_construct,
                          std::make_tuple(stream_exec->platform(),
                                          stream_exec->device_ordinal()),
                          std::make_tuple())
                .first;
  return tensorflow::mutex_lock{it->second};
}

}  // anonymous namespace

// We could have caching here so that we don't redo this work for two identical
// convolutions.  Unfortunately our cache key would have to be a tuple
// containing the protos passed to this function, and we have no utility for
// hashing protos.  We could write our own hash functions, but they'd silently
// break if we ever added a field to one of the protos.  Perhaps we could hack
// using the binary-encoded proto as the hash key, on the assumption that two
// protos being binary-equal is a sufficient, if not necessary, condition for
// proper equality.  But that would still leave us open to having unnecessary
// cache misses and doing extra work.  Overall, caching doesn't seem worth the
// trouble, but we may want to revisit this if we ever find a model where
// caching would speed up compilation a lot.
StatusOr<std::tuple<int64, bool, int64>>
CudnnConvolutionAlgorithmPicker::PickBestAlgorithm(
    CudnnConvKind kind, const Shape& input_shape, const Shape& filter_shape,
    const Shape& output_shape, const Window& window,
    const ConvolutionDimensionNumbers& dnums, HloInstruction* instr) {
  CHECK_EQ(input_shape.element_type(), filter_shape.element_type());
  CHECK_EQ(input_shape.element_type(), output_shape.element_type());
  // TODO(timshen): for now only check fp16. It can be expanded to other types,
  // with some work on the HLO routines.
  const bool cross_check_enabled = input_shape.element_type() == xla::F16;

  // Don't run this function concurrently on the same GPU.
  //
  // This is a bit of a hack and doesn't protect us against arbitrary concurrent
  // use of a GPU, but it's sufficient to let us compile two HLO modules
  // concurrently and then run them sequentially.
  tensorflow::mutex_lock lock = LockGpu(stream_exec_);

  // Create a stream for us to do our work on.
  se::Stream stream{stream_exec_};
  stream.Init();
  const auto device_ordinal = stream_exec_->device_ordinal();

  // allocator either points to this->allocator_ or, if that's null, to a
  // StreamExecutorMemoryAllocator for stream_exec_.
  DeviceMemoryAllocator* allocator;
  optional<StreamExecutorMemoryAllocator> se_allocator;
  if (allocator_ != nullptr) {
    allocator = allocator_;
  } else {
    se_allocator.emplace(
        stream_exec_->platform(),
        tensorflow::gtl::ArraySlice<se::StreamExecutor*>({stream_exec_}));
    allocator = &*se_allocator;
  }

  // Allocate space for the input, filter, and output of the convolution.  We
  // use a ScratchAllocator for this instead of calling allocator_ directly so
  // that our allocations don't leak.
  ScratchAllocator input_output_allocator(device_ordinal, allocator);
  TF_ASSIGN_OR_RETURN(DeviceMemoryBase input_buf,
                      input_output_allocator.AllocateBytes(
                          &stream, ShapeUtil::ByteSizeOf(input_shape)));
  TF_ASSIGN_OR_RETURN(DeviceMemoryBase filter_buf,
                      input_output_allocator.AllocateBytes(
                          &stream, ShapeUtil::ByteSizeOf(filter_shape)));
  TF_ASSIGN_OR_RETURN(DeviceMemoryBase output_buf,
                      input_output_allocator.AllocateBytes(
                          &stream, ShapeUtil::ByteSizeOf(output_shape)));

  if (cross_check_enabled) {
    // Broadcast a constant to the buffer, instead of zeroing the buffer. A
    // non-zero constant is useful for the cross checking, because zero-inputs
    // may not always reveal the bugs.
    const auto initialize_f16 = [&stream](DeviceMemoryBase buffer) {
      CHECK_EQ(0, (uintptr_t)buffer.opaque() % 4);
      size_t left_over_bytes = buffer.size() % 4;
      CHECK_EQ(0, left_over_bytes % 2);

      constexpr float kBroadcastedConstant = 0.1f;
      Eigen::half halfs[2] = {Eigen::half(kBroadcastedConstant),
                              Eigen::half(kBroadcastedConstant)};
      uint32 bits;
      static_assert(sizeof(bits) == sizeof(halfs), "");
      memcpy(&bits, halfs, sizeof(bits));

      size_t aligned_size = buffer.size() / 4 * 4;
      stream.ThenMemset32(&buffer, bits, aligned_size);

      DeviceMemoryBase left_over(
          static_cast<char*>(buffer.opaque()) + aligned_size, left_over_bytes);
      stream.ThenMemcpy(&left_over, halfs, left_over_bytes);
    };
    initialize_f16(input_buf);
    initialize_f16(filter_buf);
    initialize_f16(output_buf);
  } else {
    // Although we don't have evidence this matters, zero out the buffers before
    // autotuning.  It's conceivable that using uninitialized memory as the
    // inputs might affect performance if e.g. the inputs contain denormals, and
    // this is easy enough.
    stream.ThenMemZero(&input_buf, input_buf.size())
        .ThenMemZero(&filter_buf, filter_buf.size())
        .ThenMemZero(&output_buf, output_buf.size());
  }
  TF_RETURN_IF_ERROR(stream.BlockHostUntilDone());

  DeviceMemoryBase* result_buf = [&] {
    switch (kind) {
      case CudnnConvKind::kBackwardFilter:
        return &filter_buf;
      case CudnnConvKind::kBackwardInput:
        return &input_buf;
      case CudnnConvKind::kForward:
        return &output_buf;
    }
  }();

  const bool use_winograd_nonfused = ShouldIncludeWinogradNonfusedAlgo(
      input_shape, output_shape, dnums, stream_exec_);
  se::dnn::ProfileResult best_result;
  int64 best_result_bytes_used = 0;

  optional<F16BufferComparator> comparator;
  // Use the first algorithm that's supported as reference. There isn't a
  // particular reason to use it, as any algorithm sufficies. It doesn't make
  // this algorithm considered correct, though.
  optional<AlgorithmDesc> first_algorithm;
  for (const AlgorithmDesc& alg :
       GetAlgorithms(kind, use_winograd_nonfused, stream_exec_)) {
    ScratchAllocator scratch_allocator(device_ordinal, allocator);
    se::dnn::ProfileResult profile_result;
    VLOG(3) << "Trying algorithm " << AlgorithmToString(alg) << " for "
            << instr->ToString();

    bool launch_ok =
        RunCudnnConvolution(kind, input_shape, filter_shape, output_shape,
                            input_buf, filter_buf, output_buf,
                            &scratch_allocator, window, dnums,
                            AlgorithmConfig(alg), &stream, &profile_result)
            .ok();

    if (launch_ok && profile_result.is_valid()) {
      const bool crash_on_checking_failure =
          instr->GetModule()
              ->config()
              .debug_options()
              .xla_gpu_crash_on_verification_failures();
      if (comparator.has_value()) {
        StatusOr<bool> result = comparator->CompareEqual(
            se::DeviceMemory<Eigen::half>(*result_buf));
        if (!result.ok()) {
          LOG(ERROR) << "Unable to compare "
                     << AlgorithmToString(*first_algorithm) << " against "
                     << AlgorithmToString(alg) << " for " << instr->ToString()
                     << ": " << result.status();
          CHECK(!crash_on_checking_failure);
        } else if (!result.ValueOrDie()) {
          LOG(ERROR) << "Results mismatch between different convolution "
                        "algorithms. This is likely a bug in convolution, or "
                        "an excessive loss of precision in convolution. "
                     << instr->ToString() << " for "
                     << AlgorithmToString(*first_algorithm) << " vs "
                     << AlgorithmToString(alg);
          CHECK(!crash_on_checking_failure);
        }
      } else if (cross_check_enabled) {
        auto comp = F16BufferComparator::Create(
            se::DeviceMemory<Eigen::half>(*result_buf), compiler_, allocator,
            &stream);
        if (comp.ok()) {
          comparator.emplace(comp.ConsumeValueOrDie());
          first_algorithm.emplace(alg);
        } else {
          LOG(ERROR) << "Fail to initialize buffer comparator: "
                     << comp.status() << ", instruction: " << instr->ToString();
          CHECK(!crash_on_checking_failure);
        }
      }
      int64 scratch_bytes_used = scratch_allocator.TotalAllocatedBytes();
      VLOG(3) << "Run of algorithm " << AlgorithmToString(alg)
              << " succeeded, taking " << profile_result.elapsed_time_in_ms()
              << "ms and using " << NumBytesToString(scratch_bytes_used)
              << " of scratch (Best result: "
              << best_result.elapsed_time_in_ms() << "ms, "
              << NumBytesToString(best_result_bytes_used) << " of scratch)";
      if (profile_result.elapsed_time_in_ms() <
          best_result.elapsed_time_in_ms()) {
        best_result = profile_result;
        best_result_bytes_used = scratch_bytes_used;
      }
    } else {
      VLOG(3) << "Run of algorithm " << AlgorithmToString(alg) << " failed.";
    }
  }
  if (best_result.is_valid()) {
    VLOG(2) << "Best algorithm for " << instr->ToString() << ": "
            << AlgorithmToString(best_result.algorithm()) << ", takes "
            << best_result.elapsed_time_in_ms() << "ms, and uses "
            << best_result_bytes_used << "B of scratch memory.";
    return std::make_tuple(best_result.algorithm().algo_id(),
                           best_result.algorithm().tensor_ops_enabled(),
                           best_result_bytes_used);
  }

  return InternalError(
      "All algorithms tried for convolution %s failed.  Falling back to "
      "default algorithm.",
      instr->ToString());
}

StatusOr<bool> CudnnConvolutionAlgorithmPicker::RunOnInstruction(
    HloInstruction* instr) {
  CHECK(IsCustomCallToDnnConvolution(*instr));

  const auto& call_target = instr->custom_call_target();
  const auto& lhs_shape = instr->operand(0)->shape();
  const auto& rhs_shape = instr->operand(1)->shape();
  const auto& conv_result_shape = instr->shape().tuple_shapes(0);
  StatusOr<std::tuple<int64, bool, int64>> alg_scratch_and_tc;
  if (call_target == kCudnnConvForwardCallTarget) {
    alg_scratch_and_tc =
        PickBestAlgorithm(CudnnConvKind::kForward, /*input_shape=*/lhs_shape,
                          /*filter_shape=*/rhs_shape,
                          /*output_shape=*/conv_result_shape, instr->window(),
                          instr->convolution_dimension_numbers(), instr);
  } else if (call_target == kCudnnConvBackwardInputCallTarget) {
    alg_scratch_and_tc = PickBestAlgorithm(
        CudnnConvKind::kBackwardInput, /*input_shape=*/conv_result_shape,
        /*filter_shape=*/rhs_shape, /*output_shape=*/lhs_shape, instr->window(),
        instr->convolution_dimension_numbers(), instr);
  } else if (call_target == kCudnnConvBackwardFilterCallTarget) {
    alg_scratch_and_tc = PickBestAlgorithm(
        CudnnConvKind::kBackwardFilter, /*input_shape=*/lhs_shape,
        /*filter_shape=*/conv_result_shape, /*output_shape=*/rhs_shape,
        instr->window(), instr->convolution_dimension_numbers(), instr);
  } else {
    LOG(FATAL) << "Unknown custom call target for cudnn conv: "
               << instr->ToString();
  }

  if (!alg_scratch_and_tc.ok()) {
    LOG(ERROR) << alg_scratch_and_tc.status();
    return false;
  }

  int64 algorithm;
  bool tensor_ops_enabled;
  int64 scratch_bytes;

  std::tie(algorithm, tensor_ops_enabled, scratch_bytes) =
      alg_scratch_and_tc.ConsumeValueOrDie();

  VLOG(1) << "Setting cudnn conv to use algorithm " << algorithm << " and "
          << NumBytesToString(scratch_bytes)
          << " of scratch memory: " << instr->ToString()
          << " tensor_ops_enabled: " << tensor_ops_enabled;

  // Replace instr with a new CustomCall which has the correct algorithm, and
  // whose output shape has the appropriate amount of scratch memory.
  HloComputation* computation = instr->parent();
  Shape new_call_shape =
      ShapeUtil::MakeTupleShape({instr->shape().tuple_shapes(0),
                                 ShapeUtil::MakeShape(U8, {scratch_bytes})});

  CudnnConvBackendConfig backend_config;
  backend_config.set_algorithm(algorithm);
  backend_config.set_tensor_ops_enabled(tensor_ops_enabled);

  HloInstruction* new_call =
      computation->AddInstruction(HloInstruction::CreateCustomCall(
          new_call_shape,
          {instr->mutable_operand(0), instr->mutable_operand(1)},
          instr->custom_call_target()));
  new_call->set_window(instr->window());
  new_call->set_convolution_dimension_numbers(
      instr->convolution_dimension_numbers());
  TF_RETURN_IF_ERROR(new_call->set_backend_config(backend_config));

  // Repackage new_call so it has the same shape as the original call, namely
  // (conv_result, u8[0]).
  HloInstruction* new_tuple =
      computation->AddInstruction(HloInstruction::CreateTuple(
          {computation->AddInstruction(HloInstruction::CreateGetTupleElement(
               new_call_shape.tuple_shapes(0), new_call, 0)),
           computation->AddInstruction(HloInstruction::CreateConstant(
               LiteralUtil::CreateR1<uint8>({})))}));

  TF_RETURN_IF_ERROR(instr->parent()->ReplaceInstruction(instr, new_tuple));
  return true;
}

StatusOr<bool> CudnnConvolutionAlgorithmPicker::RunOnComputation(
    HloComputation* computation) {
  std::vector<HloInstruction*> convs;
  for (auto* instr : computation->instructions()) {
    if (IsCustomCallToDnnConvolution(*instr)) {
      convs.push_back(instr);
    }
  }

  bool changed = false;
  for (auto* instr : convs) {
    TF_ASSIGN_OR_RETURN(bool result, RunOnInstruction(instr));
    changed |= result;
  }
  return changed;
}

StatusOr<bool> CudnnConvolutionAlgorithmPicker::Run(HloModule* module) {
  bool changed = false;
  for (HloComputation* computation : module->MakeNonfusionComputations()) {
    TF_ASSIGN_OR_RETURN(bool result, RunOnComputation(computation));
    changed |= result;
  }
  return changed;
}

}  // namespace gpu
}  // namespace xla
