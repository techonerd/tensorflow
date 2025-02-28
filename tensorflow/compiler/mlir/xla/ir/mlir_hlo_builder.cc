/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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
#include "tensorflow/compiler/mlir/xla/ir/mlir_hlo_builder.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/raw_ostream.h"
#include "mlir/IR/Builders.h"  // from @llvm-project
#include "mlir/IR/BuiltinTypes.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/hlo/include/mlir-hlo/Dialect/mhlo/IR/hlo_ops.h"
#include "tensorflow/compiler/mlir/xla/attribute_importer.h"
#include "tensorflow/compiler/mlir/xla/hlo_function_importer.h"
#include "tensorflow/compiler/mlir/xla/hlo_utils.h"
#include "tensorflow/compiler/mlir/xla/type_to_shape.h"
#include "tensorflow/compiler/xla/comparison_util.h"
#include "tensorflow/compiler/xla/service/hlo_module.h"
#include "tensorflow/compiler/xla/service/shape_inference.h"
#include "tensorflow/compiler/xla/util.h"

namespace xla {

static std::string GetMlirOpName(HloOpcode opcode) {
  std::string op_name = HloOpcodeString(opcode);
  absl::c_replace(op_name, '-', '_');
  return mlir::mhlo::MhloDialect::getDialectNamespace().str() + "." + op_name;
}

static std::string ToString(mlir::Type ty) {
  std::string str;
  llvm::raw_string_ostream sstream(str);
  ty.print(sstream);
  sstream.flush();
  return str;
}

// Returns 1D 64-bit dense elements attribute with the given values.
static mlir::DenseIntElementsAttr GetI64ElementsAttr(
    absl::Span<const int64> values, mlir::Builder* builder) {
  auto ty = mlir::RankedTensorType::get({static_cast<int64_t>(values.size())},
                                        builder->getIntegerType(64));
  return mlir::DenseIntElementsAttr::get(
      ty, llvm::makeArrayRef(values.data(), values.size()));
}

static mlir::DenseIntElementsAttr ConvertPadding(
    absl::Span<const std::pair<int64_t, int64_t>> padding,
    mlir::Builder* builder) {
  llvm::SmallVector<int64_t, 8> elements;
  elements.reserve(padding.size() * 2);
  for (const auto& vals : padding) {
    elements.push_back(vals.first);
    elements.push_back(vals.second);
  }
  auto ty = mlir::RankedTensorType::get(
      {static_cast<int64_t>(padding.size()), 2}, builder->getIntegerType(64));
  return mlir::DenseIntElementsAttr::get(ty, elements);
}

MlirHloBuilder::~MlirHloBuilder() = default;

StatusOr<XlaOp> MlirHloBuilder::MakeXlaOp(mlir::Value val) {
  mlir::Type ty = val.getType();
  auto shape = std::make_unique<Shape>(TypeToShape(ty));
  if (shape->element_type() == PrimitiveType::PRIMITIVE_TYPE_INVALID) {
    return InvalidArgument("unsupported type: %s", ToString(ty).c_str());
  }

  int64_t handle = reinterpret_cast<int64_t>(val.getAsOpaquePointer());
  handle_to_shape_[handle] = std::move(shape);
  return XlaOp(handle, this);
}

XlaOp MlirHloBuilder::ConstantLiteral(const LiteralSlice& literal) {
  return ReportErrorOrReturn([&]() -> StatusOr<XlaOp> {
    TF_ASSIGN_OR_RETURN(mlir::DenseElementsAttr attr,
                        CreateDenseElementsAttrFromLiteral(literal, builder_));
    auto op = builder_.create<mlir::mhlo::ConstOp>(loc_, attr);
    return MakeXlaOp(op);
  });
}

StatusOr<XlaOp> MlirHloBuilder::ConvGeneralDilatedInternal(
    const Shape& shape, XlaOp lhs, XlaOp rhs, const Window& window,
    absl::Span<const int64> window_strides,
    absl::Span<const std::pair<int64, int64>> padding,
    absl::Span<const int64> lhs_dilation, absl::Span<const int64> rhs_dilation,
    const ConvolutionDimensionNumbers& dimension_numbers,
    int64 feature_group_count, int64 batch_group_count,
    const PrecisionConfig* precision_config) {
  TF_ASSIGN_OR_RETURN(mlir::Type ty, ConvertShapeToType<mlir::RankedTensorType>(
                                         shape, builder_));
  mlir::ArrayAttr config_attr;
  if (precision_config)
    config_attr = ConvertPrecisionConfig(precision_config, &builder_);
  auto op = builder_.create<mlir::mhlo::ConvOp>(
      loc_, ty, GetValue(lhs), GetValue(rhs),
      GetI64ElementsAttr(window_strides, &builder_),
      ConvertPadding(padding, &builder_),
      GetI64ElementsAttr(lhs_dilation, &builder_),
      GetI64ElementsAttr(rhs_dilation, &builder_),
      /*window_reversal=*/nullptr,
      ConvertConvDimensionNumbers(dimension_numbers, &builder_),
      builder_.getI64IntegerAttr(feature_group_count),
      builder_.getI64IntegerAttr(batch_group_count), config_attr);
  return MakeXlaOp(op);
}

StatusOr<XlaOp> MlirHloBuilder::FftInternal(
    const Shape& shape, XlaOp operand, FftType fft_type,
    absl::Span<const int64> fft_length) {
  TF_ASSIGN_OR_RETURN(mlir::Type ty, ConvertShapeToType<mlir::RankedTensorType>(
                                         shape, builder_));
  auto op = builder_.create<mlir::mhlo::FftOp>(
      loc_, ty, GetValue(operand),
      builder_.getStringAttr(FftType_Name(fft_type)),
      GetI64ElementsAttr(fft_length, &builder_));
  return MakeXlaOp(op);
}

StatusOr<XlaOp> MlirHloBuilder::CustomCallInternal(
    const string& call_target_name, absl::Span<const XlaOp> operands,
    const Shape& shape, const string& opaque,
    absl::optional<absl::Span<const Shape>> operand_shapes_with_layout,
    bool has_side_effect,
    absl::Span<const std::pair<ShapeIndex, std::pair<int64, ShapeIndex>>>
        output_operand_aliasing,
    const Literal* literal, absl::optional<Window> window,
    absl::optional<ConvolutionDimensionNumbers> dnums) {
  if (operand_shapes_with_layout.has_value())
    return Unimplemented(
        "CustomCall doesn't support operands shapes with layout");
  TF_ASSIGN_OR_RETURN(mlir::Type ty, ConvertShapeToType<mlir::RankedTensorType>(
                                         shape, builder_));
  TF_RET_CHECK(output_operand_aliasing.empty())
      << "MLIR CustomCallOp does not support output_operand_aliasing yet";
  TF_RET_CHECK(literal == nullptr)
      << "MLIR CustomCallOp does not support literal yet";
  TF_RET_CHECK(!window.has_value())
      << "MLIR CustomCallOp does not support ConvolutionDimensionNumbers yet";
  TF_RET_CHECK(!dnums.has_value())
      << "MLIR CustomCallOp does not support ConvolutionDimensionNumbers yet";
  auto op = builder_.create<mlir::mhlo::CustomCallOp>(
      loc_, ty, GetValues(operands), builder_.getStringAttr(call_target_name),
      /*has_side_effect=*/builder_.getBoolAttr(has_side_effect),
      builder_.getStringAttr(opaque));
  return MakeXlaOp(op.getResult(0));
}

StatusOr<XlaOp> MlirHloBuilder::ReduceInternal(
    const Shape& shape, absl::Span<const XlaOp> all_operands,
    const XlaComputation& computation,
    absl::Span<const int64> dimensions_to_reduce) {
  // Reduce takes two set of variadic operands inputs and init_values.
  // all_operands contains both of these so split operands into two parts.
  int64_t num_args = all_operands.size() / 2;
  auto op = builder_.create<mlir::mhlo::ReduceOp>(
      loc_, GetValues(all_operands.first(num_args)),
      GetValues(all_operands.subspan(num_args)),
      GetI64ElementsAttr(dimensions_to_reduce, &builder_));
  TF_RETURN_IF_ERROR(ImportComputation(computation.proto(), &op.body()));
  if (op.getNumResults() == 1) return MakeXlaOp(op.getResult(0));
  auto tuple = builder_.create<mlir::mhlo::TupleOp>(loc_, op.getResults());
  return MakeXlaOp(tuple);
}

StatusOr<XlaOp> MlirHloBuilder::ReduceWindowInternal(
    const Shape& shape, XlaOp operand, XlaOp init_value,
    const XlaComputation& computation, Window window) {
  TF_ASSIGN_OR_RETURN(mlir::Type ty, ConvertShapeToType<mlir::RankedTensorType>(
                                         shape, builder_));
  llvm::SmallVector<int64, 4> sizes, strides, base_dilations, win_dilations;
  llvm::SmallVector<int64, 8> padding;
  for (const auto& dim : window.dimensions()) {
    sizes.push_back(dim.size());
    strides.push_back(dim.stride());
    base_dilations.push_back(dim.base_dilation());
    win_dilations.push_back(dim.window_dilation());
    padding.push_back(dim.padding_low());
    padding.push_back(dim.padding_high());
  }
  auto padding_ty =
      mlir::RankedTensorType::get({static_cast<int64_t>(padding.size()) / 2, 2},
                                  builder_.getIntegerType(64));
  auto op = builder_.create<mlir::mhlo::ReduceWindowOp>(
      loc_, ty, GetValue(operand), GetValue(init_value),
      GetI64ElementsAttr(sizes, &builder_),
      GetI64ElementsAttr(strides, &builder_),
      GetI64ElementsAttr(base_dilations, &builder_),
      GetI64ElementsAttr(win_dilations, &builder_),
      mlir::DenseIntElementsAttr::get(padding_ty, padding));
  TF_RETURN_IF_ERROR(ImportComputation(computation.proto(), &op.body()));
  return MakeXlaOp(op.getResult(0));
}

XlaOp MlirHloBuilder::Iota(const Shape& shape, int64 iota_dimension) {
  return ReportErrorOrReturn([&]() -> StatusOr<XlaOp> {
    TF_ASSIGN_OR_RETURN(
        mlir::Type ty,
        ConvertShapeToType<mlir::RankedTensorType>(shape, builder_));
    auto op = builder_.create<mlir::mhlo::IotaOp>(
        loc_, ty,
        builder_.getIntegerAttr(builder_.getI64Type(), iota_dimension));
    return MakeXlaOp(op);
  });
}

StatusOr<XlaOp> MlirHloBuilder::BitcastConvertTypeInternal(const Shape& shape,
                                                           XlaOp operand) {
  TF_ASSIGN_OR_RETURN(mlir::Type ty, ConvertShapeToType<mlir::RankedTensorType>(
                                         shape, builder_));
  auto op = builder_.create<mlir::mhlo::BitcastConvertOp>(loc_, ty,
                                                          GetValue(operand));
  return MakeXlaOp(op);
}

StatusOr<XlaOp> MlirHloBuilder::TransposeInternal(
    const Shape& shape, XlaOp operand, absl::Span<const int64> permutation) {
  TF_ASSIGN_OR_RETURN(mlir::Type ty, ConvertShapeToType<mlir::RankedTensorType>(
                                         shape, builder_));
  auto op = builder_.create<mlir::mhlo::TransposeOp>(
      loc_, ty, GetValue(operand), GetI64ElementsAttr(permutation, &builder_));
  return MakeXlaOp(op);
}

StatusOr<XlaOp> MlirHloBuilder::RevInternal(
    const Shape& shape, XlaOp operand, absl::Span<const int64> dimensions) {
  TF_ASSIGN_OR_RETURN(mlir::Type ty, ConvertShapeToType<mlir::RankedTensorType>(
                                         shape, builder_));
  auto op = builder_.create<mlir::mhlo::ReverseOp>(
      loc_, ty, GetValue(operand), GetI64ElementsAttr(dimensions, &builder_));
  return MakeXlaOp(op);
}

StatusOr<XlaOp> MlirHloBuilder::SortInternal(const Shape& shape,
                                             absl::Span<const XlaOp> operands,
                                             const XlaComputation& comparator,
                                             int64 dimension, bool is_stable) {
  TF_ASSIGN_OR_RETURN(mlir::Type ty, ConvertShapeToType<mlir::RankedTensorType>(
                                         shape, builder_));
  llvm::SmallVector<mlir::Type, 4> sort_types = {ty};
  if (auto tuple_ty = ty.dyn_cast<mlir::TupleType>()) {
    sort_types = llvm::to_vector<6>(tuple_ty.getTypes());
  }

  auto op = builder_.create<mlir::mhlo::SortOp>(
      loc_, sort_types, GetValues(operands),
      builder_.getI64IntegerAttr(dimension), builder_.getBoolAttr(is_stable));
  TF_RETURN_IF_ERROR(ImportComputation(comparator.proto(), &op.comparator()));

  if (ty.isa<mlir::TupleType>()) {
    auto tuple = builder_.create<mlir::mhlo::TupleOp>(loc_, op.getResults());
    return MakeXlaOp(tuple);
  }

  return MakeXlaOp(op.getResult(0));
}

StatusOr<XlaOp> MlirHloBuilder::WhileInternal(const Shape& shape,
                                              const XlaComputation& condition,
                                              const XlaComputation& body,
                                              XlaOp init) {
  TF_ASSIGN_OR_RETURN(mlir::Type ty, ConvertShapeToType<mlir::RankedTensorType>(
                                         shape, builder_));
  auto op = builder_.create<mlir::mhlo::WhileOp>(loc_, ty, GetValue(init));
  TF_RETURN_IF_ERROR(ImportComputation(condition.proto(), &op.cond()));
  TF_RETURN_IF_ERROR(ImportComputation(body.proto(), &op.body()));
  return MakeXlaOp(op);
}

StatusOr<XlaOp> MlirHloBuilder::ReducePrecisionInternal(
    const Shape& shape, XlaOp operand, const int exponent_bits,
    const int mantissa_bits) {
  TF_ASSIGN_OR_RETURN(mlir::Type ty, ConvertShapeToType<mlir::RankedTensorType>(
                                         shape, builder_));
  auto op = builder_.create<mlir::mhlo::ReducePrecisionOp>(
      loc_, ty, GetValue(operand), builder_.getI32IntegerAttr(exponent_bits),
      builder_.getI32IntegerAttr(mantissa_bits));
  return MakeXlaOp(op);
}

StatusOr<XlaOp> MlirHloBuilder::GatherInternal(
    const Shape& shape, XlaOp input, XlaOp start_indices,
    const GatherDimensionNumbers& dimension_numbers,
    absl::Span<const int64> slice_sizes, bool indices_are_sorted) {
  TF_ASSIGN_OR_RETURN(mlir::Type ty, ConvertShapeToType<mlir::RankedTensorType>(
                                         shape, builder_));
  auto op = builder_.create<mlir::mhlo::GatherOp>(
      loc_, ty, GetValue(input), GetValue(start_indices),
      ConvertGatherDimensionNumbers(dimension_numbers, &builder_),
      GetI64ElementsAttr(slice_sizes, &builder_));
  return MakeXlaOp(op);
}

StatusOr<XlaOp> MlirHloBuilder::ScatterInternal(
    const Shape& shape, XlaOp input, XlaOp scatter_indices, XlaOp updates,
    const XlaComputation& update_computation,
    const ScatterDimensionNumbers& dimension_numbers, bool indices_are_sorted,
    bool unique_indices) {
  TF_ASSIGN_OR_RETURN(mlir::Type ty, ConvertShapeToType<mlir::RankedTensorType>(
                                         shape, builder_));
  auto op = builder_.create<mlir::mhlo::ScatterOp>(
      loc_, ty, GetValue(input), GetValue(scatter_indices), GetValue(updates),
      ConvertScatterDimensionNumbers(dimension_numbers, &builder_),
      builder_.getBoolAttr(indices_are_sorted),
      builder_.getBoolAttr(unique_indices));

  TF_RETURN_IF_ERROR(
      ImportComputation(update_computation.proto(), &op.update_computation()));
  return MakeXlaOp(op);
}

StatusOr<XlaOp> MlirHloBuilder::SetDimensionSizeInternal(const Shape& shape,
                                                         XlaOp operand,
                                                         XlaOp val,
                                                         int64 dimension) {
  TF_ASSIGN_OR_RETURN(mlir::Type ty, ConvertShapeToType<mlir::RankedTensorType>(
                                         shape, builder_));
  auto op = builder_.create<mlir::mhlo::SetDimensionSizeOp>(
      loc_, ty, GetValue(operand), GetValue(val),
      builder_.getI64IntegerAttr(dimension));
  return MakeXlaOp(op);
}

StatusOr<XlaOp> MlirHloBuilder::RngOpInternal(
    RandomDistribution distribution, absl::Span<const XlaOp> parameters,
    const Shape& shape) {
  // TODO(hinsu): Introduce RngOp in the HLO dialect in MLIR and then RngUniform
  // and RngNormal can be mapped to the new op.
  std::string op_name;
  if (distribution == xla::RandomDistribution::RNG_UNIFORM) {
    op_name = "mhlo.rng_uniform";
  } else {
    TF_RET_CHECK(distribution == xla::RandomDistribution::RNG_NORMAL)
        << "Unexpected distribution: " << distribution;
    op_name = "mhlo.rng_normal";
  }

  if (shape.is_dynamic())
    return Unimplemented("RngOp with dynamic dims not supported");
  llvm::SmallVector<XlaOp, 3> operands;
  operands.append(parameters.begin(), parameters.end());
  operands.push_back(
      ConstantLiteral(LiteralUtil::CreateR1<int64>(shape.dimensions())));
  return CreateOp(op_name, shape, operands);
}

StatusOr<XlaOp> MlirHloBuilder::RngBitGeneratorInternal(
    const Shape& full_result_shape, RandomAlgorithm algorithm,
    XlaOp initial_state) {
  TF_ASSIGN_OR_RETURN(mlir::Type ty, ConvertShapeToType<mlir::RankedTensorType>(
                                         full_result_shape, builder_));
  auto op = builder_.create<mlir::mhlo::RngBitGeneratorOp>(
      loc_, ty, builder_.getI32IntegerAttr(algorithm), GetValue(initial_state));
  return MakeXlaOp(op);
}

StatusOr<XlaOp> MlirHloBuilder::ReshapeInternal(const Shape& shape,
                                                XlaOp operand,
                                                int64 inferred_dimension) {
  TF_RETURN_IF_ERROR(first_error());

  if (inferred_dimension != -1)
    return Unimplemented("inferred_dimension not yet supported for Reshape op");
  TF_ASSIGN_OR_RETURN(mlir::Type ty, ConvertShapeToType<mlir::RankedTensorType>(
                                         shape, builder_));
  mlir::Value value = GetValue(operand);
  auto op = builder_.create<mlir::mhlo::ReshapeOp>(loc_, ty, value);
  return MakeXlaOp(op.getResult());
}

StatusOr<XlaOp> MlirHloBuilder::DotGeneralInternal(
    const Shape& shape, XlaOp lhs, XlaOp rhs,
    const DotDimensionNumbers& dimension_number,
    const PrecisionConfig* precision_config) {
  TF_ASSIGN_OR_RETURN(mlir::Type ty, ConvertShapeToType<mlir::RankedTensorType>(
                                         shape, builder_));
  auto op = builder_.create<mlir::mhlo::DotGeneralOp>(
      loc_, ty, GetValue(lhs), GetValue(rhs),
      ConvertDotDimensionNumbers(dimension_number, &builder_),
      ConvertPrecisionConfig(precision_config, &builder_));
  return MakeXlaOp(op.getResult());
}

StatusOr<XlaOp> MlirHloBuilder::InDimBroadcast(
    const Shape& shape, XlaOp operand,
    absl::Span<const int64> broadcast_dimensions) {
  TF_RETURN_IF_ERROR(first_error());
  TF_ASSIGN_OR_RETURN(mlir::Type ty, ConvertShapeToType<mlir::RankedTensorType>(
                                         shape, builder_));
  mlir::Value value = GetValue(operand);
  auto op = builder_.create<mlir::mhlo::BroadcastInDimOp>(
      loc_, ty, value, GetI64ElementsAttr(broadcast_dimensions, &builder_));
  return MakeXlaOp(op.getResult());
}

StatusOr<XlaOp> MlirHloBuilder::AddInstruction(
    HloInstructionProto&& instr, HloOpcode opcode,
    absl::Span<const XlaOp> operands) {
  return Unimplemented("MlirHloBuilder does not support op %s",
                       HloOpcodeString(opcode));
}

StatusOr<XlaOp> MlirHloBuilder::Compare(const Shape& shape, XlaOp lhs,
                                        XlaOp rhs,
                                        ComparisonDirection direction,
                                        Comparison::Type type) {
  TF_ASSIGN_OR_RETURN(mlir::Type ty, ConvertShapeToType<mlir::RankedTensorType>(
                                         shape, builder_));
  auto op = builder_.create<mlir::mhlo::CompareOp>(
      loc_, ty, GetValue(lhs), GetValue(rhs),
      builder_.getStringAttr(ComparisonDirectionToString(direction)),
      builder_.getStringAttr(ComparisonTypeToString(type)));
  return MakeXlaOp(op.getResult());
}

XlaOp MlirHloBuilder::BinaryOpNoBroadcast(HloOpcode binop, const Shape& shape,
                                          XlaOp lhs, XlaOp rhs) {
  return ReportErrorOrReturn([&]() -> StatusOr<XlaOp> {
    return CreateOp(GetMlirOpName(binop), shape, {lhs, rhs});
  });
}

StatusOr<XlaOp> MlirHloBuilder::AddOpWithShape(
    HloOpcode opcode, const Shape& shape, absl::Span<const XlaOp> operands) {
  return CreateOp(GetMlirOpName(opcode), shape,
                  llvm::makeArrayRef<XlaOp>(operands.data(), operands.size()));
}

XlaOp MlirHloBuilder::CreateToken() {
  return ReportErrorOrReturn([&]() -> StatusOr<XlaOp> {
    return MakeXlaOp(builder_.create<mlir::mhlo::CreateTokenOp>(
        loc_, mlir::mhlo::TokenType::get(builder_.getContext())));
  });
}

StatusOr<XlaOp> MlirHloBuilder::TriangularSolveInternal(
    const Shape& shape, XlaOp a, XlaOp b, TriangularSolveOptions options) {
  TF_ASSIGN_OR_RETURN(
      mlir::Type result_ty,
      ConvertShapeToType<mlir::RankedTensorType>(shape, builder_));
  auto op = builder_.create<mlir::mhlo::TriangularSolveOp>(
      loc_, result_ty, GetValue(a), GetValue(b),
      builder_.getBoolAttr(options.left_side()),
      builder_.getBoolAttr(options.lower()),
      builder_.getBoolAttr(options.unit_diagonal()),
      builder_.getStringAttr(
          TriangularSolveOptions::Transpose_Name(options.transpose_a())));
  return MakeXlaOp(op);
}

StatusOr<XlaOp> MlirHloBuilder::CholeskyInternal(const Shape& shape, XlaOp a,
                                                 bool lower) {
  TF_ASSIGN_OR_RETURN(
      mlir::Type result_ty,
      ConvertShapeToType<mlir::RankedTensorType>(shape, builder_));
  auto op = builder_.create<mlir::mhlo::CholeskyOp>(
      loc_, result_ty, GetValue(a), builder_.getBoolAttr(lower));
  return MakeXlaOp(op);
}

StatusOr<XlaOp> MlirHloBuilder::InfeedWithTokenInternal(
    const Shape& infeed_instruction_shape, XlaOp token, const string& config) {
  TF_ASSIGN_OR_RETURN(mlir::Type result_type,
                      ConvertShapeToType<mlir::RankedTensorType>(
                          infeed_instruction_shape, builder_));
  mlir::ArrayAttr layout;
  return MakeXlaOp(
      builder_.create<mlir::mhlo::InfeedOp>(loc_, result_type, GetValue(token),
                                            /*infeed_config=*/config,
                                            /*layout=*/layout));
}

StatusOr<XlaOp> MlirHloBuilder::OutfeedWithTokenInternal(
    XlaOp operand, XlaOp token, const Shape& shape_with_layout,
    const string& outfeed_config) {
  auto token_type = mlir::mhlo::TokenType::get(builder_.getContext());
  return MakeXlaOp(builder_.create<mlir::mhlo::OutfeedOp>(
      loc_, token_type, GetValue(operand), GetValue(token), outfeed_config));
}

StatusOr<XlaOp> MlirHloBuilder::ConcatInDimInternal(
    const Shape& shape, absl::Span<const XlaOp> operands, int64 dimension) {
  TF_ASSIGN_OR_RETURN(
      mlir::Type result_type,
      ConvertShapeToType<mlir::RankedTensorType>(shape, builder_));
  auto mlir_operands = GetValues(operands);
  return MakeXlaOp(builder_.create<mlir::mhlo::ConcatenateOp>(
      loc_, result_type, mlir_operands, builder_.getI64IntegerAttr(dimension)));
}

StatusOr<XlaOp> MlirHloBuilder::GetTupleElementInternal(const Shape& shape,
                                                        XlaOp tuple_data,
                                                        int64 index) {
  TF_ASSIGN_OR_RETURN(
      mlir::Type result_type,
      ConvertShapeToType<mlir::RankedTensorType>(shape, builder_));
  return MakeXlaOp(builder_.create<mlir::mhlo::GetTupleElementOp>(
      loc_, result_type, GetValue(tuple_data),
      builder_.getI32IntegerAttr(index)));
}

StatusOr<XlaOp> MlirHloBuilder::SliceInternal(
    const Shape& shape, XlaOp operand, absl::Span<const int64> start_indices,
    absl::Span<const int64> limit_indices, absl::Span<const int64> strides) {
  return MakeXlaOp(builder_.create<mlir::mhlo::SliceOp>(
      loc_, GetValue(operand), GetI64ElementsAttr(start_indices, &builder_),
      GetI64ElementsAttr(limit_indices, &builder_),
      GetI64ElementsAttr(strides, &builder_)));
}

StatusOr<XlaOp> MlirHloBuilder::DynamicSliceInternal(
    const Shape& shape, XlaOp operand, absl::Span<const XlaOp> start_indices,
    absl::Span<const int64> slice_sizes) {
  TF_ASSIGN_OR_RETURN(
      mlir::Type result_ty,
      ConvertShapeToType<mlir::RankedTensorType>(shape, builder_));
  return MakeXlaOp(builder_.create<mlir::mhlo::DynamicSliceOp>(
      loc_, result_ty, GetValue(operand), GetValues(start_indices),
      GetI64ElementsAttr(slice_sizes, &builder_)));
}

StatusOr<XlaOp> MlirHloBuilder::DynamicUpdateSliceInternal(
    const Shape& shape, XlaOp operand, XlaOp update,
    absl::Span<const XlaOp> start_indices) {
  TF_ASSIGN_OR_RETURN(
      mlir::Type result_ty,
      ConvertShapeToType<mlir::RankedTensorType>(shape, builder_));
  return MakeXlaOp(builder_.create<mlir::mhlo::DynamicUpdateSliceOp>(
      loc_, result_ty, GetValue(operand), GetValue(update),
      GetValues(start_indices)));
}

StatusOr<XlaOp> MlirHloBuilder::PadInternal(
    const Shape& shape, XlaOp operand, XlaOp padding_value,
    const PaddingConfig& padding_config) {
  TF_ASSIGN_OR_RETURN(
      mlir::Type result_type,
      ConvertShapeToType<mlir::RankedTensorType>(shape, builder_));
  std::vector<int64> low;
  std::vector<int64> high;
  std::vector<int64> internal;
  for (auto& dimension : padding_config.dimensions()) {
    low.push_back(dimension.edge_padding_low());
    high.push_back(dimension.edge_padding_high());
    internal.push_back(dimension.interior_padding());
  }
  return MakeXlaOp(builder_.create<mlir::mhlo::PadOp>(
      loc_, result_type, GetValue(operand), GetValue(padding_value),
      GetI64ElementsAttr(low, &builder_), GetI64ElementsAttr(high, &builder_),
      GetI64ElementsAttr(internal, &builder_)));
}

StatusOr<XlaOp> MlirHloBuilder::TupleInternal(
    const Shape& shape, absl::Span<const XlaOp> elements) {
  mlir::SmallVector<mlir::Value, 4> operands;
  for (auto& element : elements) {
    operands.push_back(GetValue(element));
  }
  return MakeXlaOp(builder_.create<mlir::mhlo::TupleOp>(loc_, operands));
}

StatusOr<XlaOp> MlirHloBuilder::CreateOp(
    const std::string& op_name, const Shape& shape,
    llvm::ArrayRef<XlaOp> operands,
    llvm::ArrayRef<mlir::NamedAttribute> attributes) {
  llvm::SmallVector<mlir::Value, 4> operand_values;
  operand_values.reserve(operands.size());
  for (XlaOp xla_op : operands) {
    operand_values.push_back(GetValue(xla_op));
  }
  TF_ASSIGN_OR_RETURN(mlir::Type ty, ConvertShapeToType<mlir::RankedTensorType>(
                                         shape, builder_));
  mlir::OperationState state(loc_, op_name, operand_values, {ty}, attributes);
  mlir::Operation* op = builder_.createOperation(state);
  return MakeXlaOp(op->getResult(0));
}

Status MlirHloBuilder::ImportComputation(const HloModuleProto& computation,
                                         mlir::Region* region) {
  TF_ASSIGN_OR_RETURN(auto module_config,
                      xla::HloModule::CreateModuleConfigFromProto(
                          computation, xla::DebugOptions()));
  TF_ASSIGN_OR_RETURN(auto hlo_module, xla::HloModule::CreateFromProto(
                                           computation, module_config));

  return HloFunctionImporter::ImportAsRegion(*hlo_module->entry_computation(),
                                             region, &builder_);
}

StatusOr<const Shape*> MlirHloBuilder::GetShapePtr(XlaOp op) const {
  TF_RETURN_IF_ERROR(first_error());
  TF_RETURN_IF_ERROR(CheckOpBuilder(op));
  auto it = handle_to_shape_.find(op.handle());
  if (it == handle_to_shape_.end()) {
    return InvalidArgument("No XlaOp with handle %d", op.handle());
  }
  return it->second.get();
}

}  // namespace xla
