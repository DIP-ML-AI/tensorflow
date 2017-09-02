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

#include "tensorflow/compiler/xla/service/logical_buffer_analysis.h"

#include <utility>

#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/platform/logging.h"

namespace xla {

/* static */ StatusOr<std::unique_ptr<LogicalBufferAnalysis>>
LogicalBufferAnalysis::Run(const HloModule* module) {
  std::unique_ptr<LogicalBufferAnalysis> analysis(
      new LogicalBufferAnalysis(module));
  TF_RETURN_IF_ERROR(analysis->Analyze());
  return std::move(analysis);
}

Status LogicalBufferAnalysis::Analyze() {
  // Empirically we usually have a few more logical buffers than instructions,
  // so reserve 10% more than the number of instructions to avoid frequent
  // resizes.
  logical_buffers_.clear();
  logical_buffers_.reserve((module_->NumUniqueInstructionIds() * 11) / 10);

  // We filter out fusion computations, and get to them through fusion
  // instructions. This is because it's possible to have orphaned (unreachable)
  // fusion computations, and we don't want to try to assign buffers to those.
  for (auto& computation : module_->computations()) {
    if (computation->IsFusionComputation()) {
      continue;
    }
    TF_RETURN_IF_ERROR(computation->Accept(this));
    for (auto& instruction : computation->instructions()) {
      if (instruction->opcode() != HloOpcode::kFusion) {
        continue;
      }
      TF_RETURN_IF_ERROR(instruction->fused_expression_root()->Accept(this));
    }
  }
  return Status::OK();
}

LogicalBuffer& LogicalBufferAnalysis::GetBuffer(LogicalBuffer::Id id) const {
  CHECK_GE(id, 0);
  CHECK_LT(id, logical_buffers_.size());
  return *logical_buffers_[id];
}

LogicalBuffer& LogicalBufferAnalysis::GetBuffer(HloInstruction* instruction,
                                                const ShapeIndex& index) const {
  return *output_buffers_.at(std::make_pair(instruction, index));
}

void LogicalBufferAnalysis::NewLogicalBuffer(HloInstruction* instruction,
                                             const ShapeIndex& index) {
  CHECK_EQ(logical_buffers_.size(), next_buffer_id_);
  logical_buffers_.emplace_back(
      MakeUnique<LogicalBuffer>(instruction, index, next_buffer_id_));
  output_buffers_[std::make_pair(instruction, index)] =
      logical_buffers_.back().get();

  ++next_buffer_id_;
}

Status LogicalBufferAnalysis::DefaultAction(HloInstruction* hlo_instruction) {
  // Create a logical buffer for each output of the instruction.
  ShapeUtil::ForEachSubshape(
      hlo_instruction->shape(),
      [this, hlo_instruction](const Shape& shape, const ShapeIndex& index) {
        NewLogicalBuffer(hlo_instruction, index);
      });

  return Status::OK();
}

Status LogicalBufferAnalysis::HandleGetTupleElement(
    HloInstruction* get_tuple_element, HloInstruction* operand) {
  // GetTupleElement does not create buffers.
  return Status::OK();
}

Status LogicalBufferAnalysis::HandleCopy(HloInstruction* copy) {
  // The top-level buffer (index={}) for kCopy is newly created, but all other
  // buffers (in the case of a tuple shape) come from the operand
  NewLogicalBuffer(copy, /*index=*/{});
  return Status::OK();
}

Status LogicalBufferAnalysis::HandleBitcast(HloInstruction* bitcast) {
  // A kBitcast instruction aliases its operand. That is, the buffer of its
  // result *is* the buffer of its operand.
  return Status::OK();
}

Status LogicalBufferAnalysis::HandleTuple(
    HloInstruction* tuple,
    tensorflow::gtl::ArraySlice<HloInstruction*> operands) {
  // A Tuple instruction only creates the top-level buffer.
  NewLogicalBuffer(tuple, /*index=*/{});
  return Status::OK();
}

Status LogicalBufferAnalysis::HandleSelect(HloInstruction* select,
                                           HloInstruction* /*pred*/,
                                           HloInstruction* on_true,
                                           HloInstruction* on_false) {
  // Select allocates a new buffer and then shallow copies the on_true or
  // on_false buffer into this new buffer.
  NewLogicalBuffer(select, /*index=*/{});
  return Status::OK();
}

}  // namespace xla
