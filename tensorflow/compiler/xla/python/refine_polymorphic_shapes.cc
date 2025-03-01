/* Copyright 2023 The JAX Authors.

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
#include "tensorflow/compiler/xla/python/refine_polymorphic_shapes.h"

#include <memory>
#include <string>

#include "absl/status/status.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/raw_ostream.h"
#include "mlir/Bytecode/BytecodeWriter.h"  // from @llvm-project
#include "mlir/Dialect/Func/Extensions/AllExtensions.h"  // from @llvm-project
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/IR/ValueRange.h"  // from @llvm-project
#include "mlir/IR/Verifier.h"  // from @llvm-project
#include "mlir/Parser/Parser.h"  // from @llvm-project
#include "mlir/Pass/PassManager.h"  // from @llvm-project
#include "mlir/Support/LogicalResult.h"  // from @llvm-project
#include "mlir/Transforms/Passes.h"  // from @llvm-project
#include "stablehlo/dialect/ChloOps.h"  // from @stablehlo
#include "stablehlo/dialect/StablehloOps.h"  // from @stablehlo
#include "stablehlo/transforms/Passes.h"  // from @stablehlo
#include "tensorflow/compiler/xla/mlir/utils/error_util.h"
#include "tensorflow/tsl/platform/errors.h"

namespace xla {

namespace {

constexpr absl::string_view shapeAssertionName = "shape_assertion";
constexpr absl::string_view errorMessageAttrName = "error_message";
// We bound the number of error_message_inputs for using llvm::formatv
constexpr int maxErrorMessageInputs = 4;

// This pass is needed when we have shape assertions. A shape assertion is
// represented via the `stablehlo.custom_call @shape_assertion`
// custom call, and represents an assertion that the first operand
// (`assert_what`) evaluates to `true`. The custom call also has an
// `error_message` string attribute, and a variadic number
// of integer scalar operands that may be used to format the error message.
// The `error_message` may contain format specifiers `{0}`, `{1}`, ..., that
// are replaced with the values of the error message inputs. The formatting is
// done with the `llvm::formatv` function
// (https://llvm.org/docs/ProgrammersManual.html#formatting-strings-the-formatv-function).
//
struct CheckShapeAssertionsPass
    : public mlir::PassWrapper<CheckShapeAssertionsPass,
                               mlir::OperationPass<mlir::func::FuncOp>> {
 public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(CheckShapeAssertionsPass)

  explicit CheckShapeAssertionsPass(bool enable_shape_assertions = true)
      : PassWrapper() {
    this->enable_shape_assertions = enable_shape_assertions;
  }

  CheckShapeAssertionsPass(const CheckShapeAssertionsPass &pass) {
    enable_shape_assertions = pass.enable_shape_assertions;
  }

 private:
  void runOnOperation() final {
    mlir::func::FuncOp func_op = getOperation();
    func_op.walk([this](mlir::stablehlo::CustomCallOp op) {
      if (!op.getCallTargetName().equals(shapeAssertionName)) return;
      if (!enable_shape_assertions) {
        op.erase();
        return;
      }
      // Check first for ill-formed assertions, rather than silently fail.
      if (mlir::failed(verifyShapeAssertion(op))) {
        signalPassFailure();
        return;
      }
      mlir::OperandRange inputs = op.getInputs();
      mlir::SmallVector<int64_t> assertWhat;
      if (mlir::failed(mlir::hlo::matchInts(inputs[0], assertWhat))) {
        op.emitError() << "expects static assert_what (operand #0)";
        signalPassFailure();
        return;
      }
      if (assertWhat[0] != 0) {
        op.erase();
        return;
      }
      llvm::StringRef errorMessage = getErrorMessage(op);
      mlir::SmallVector<int64_t> errorMessageInputs;
      for (int i = 1; i < inputs.size(); ++i) {
        mlir::SmallVector<int64_t> input;
        if (failed(mlir::hlo::matchInts(inputs[i], input))) {
          op.emitError() << "expects static error_message_input (operand #" << i
                         << ")";
          signalPassFailure();
          return;
        }
        errorMessageInputs.push_back(input[0]);
      }
      op.emitError(formatErrorMessage(errorMessage, errorMessageInputs));
      signalPassFailure();
    });
  }

  mlir::LogicalResult verifyShapeAssertion(mlir::stablehlo::CustomCallOp op) {
    if (!(1 <= op->getNumOperands() &&
          op->getNumOperands() <= 1 + maxErrorMessageInputs))
      return op.emitError() << "expects 1 <= size(operands) <= "
                            << (1 + maxErrorMessageInputs);
    int nrErrorMessageInputs = op.getNumOperands() - 1;
    if (op->getNumResults() != 0)
      return op.emitError("expects size(results) = 0");
    for (const auto &attr : op->getAttrs()) {
      if (attr.getName() != "api_version" &&
          attr.getName() != "backend_config" &&
          attr.getName() != "call_target_name" &&
          attr.getName() != "error_message" &&
          attr.getName() != "has_side_effect")
        return op.emitError()
               << attr.getName() << " is not a supported attribute";
    }
    if (!op.getBackendConfig().empty())
      return op.emitError() << "expects an empty backend_config";
    if (!op.getCallTargetName().equals(shapeAssertionName))
      return op.emitError() << "expects @shape_assertion";
    if (!op.getHasSideEffect())
      return op.emitError() << "expects has_side_effect=true";

    // input[0] (assert_what) : tensor<i1>
    auto assertWhatType =
        op.getInputs()[0].getType().dyn_cast<mlir::ShapedType>();
    if (!assertWhatType || !assertWhatType.hasRank() ||
        assertWhatType.getRank() != 0 ||
        !assertWhatType.getElementType().isSignlessInteger() ||
        assertWhatType.getElementTypeBitWidth() != 1)
      return op.emitError() << "expects assert_what (operand #0) "
                            << "to be a constant of type tensor<i1>";

    // input[1:] (error_message_inputs) : tensor<i32> or tensor<i64>
    for (int i = 0; i < nrErrorMessageInputs; ++i) {
      auto errorMessageInputType =
          op.getInputs()[i + 1].getType().dyn_cast<mlir::ShapedType>();
      if (!errorMessageInputType || !errorMessageInputType.hasRank() ||
          errorMessageInputType.getRank() != 0 ||
          !errorMessageInputType.getElementType().isSignlessInteger() ||
          (errorMessageInputType.getElementTypeBitWidth() != 32 &&
           errorMessageInputType.getElementTypeBitWidth() != 64))
        return op.emitError()
               << "expects error_message_input (operand #" << (i + 1) << ") "
               << "to be a constant of type tensor<i32> or tensor<i64>";
    }

    if (!op->hasAttr(errorMessageAttrName))
      return op.emitError() << "expects an error_message attribute";

    // error_message contains valid format specifiers.
    std::string errorMessage = getErrorMessage(op).data();
    // format specs: "{" index ["," layout] [":" format] "}"
    llvm::Regex formatSpecifierRE = llvm::Regex("{([0-9]+)[,:}]");
    do {
      mlir::SmallVector<llvm::StringRef> formatSpec;
      if (!formatSpecifierRE.match(errorMessage, &formatSpec)) {
        break;
      }
      int index = std::stoi(formatSpec[1].data());
      if (!(0 <= index && index < nrErrorMessageInputs)) {
        return op.emitError()
               << "expects error_message to contain format specifiers with "
               << "error_message_input index less than " << nrErrorMessageInputs
               << ". Found specifier " << formatSpec[0];
      }
      errorMessage = formatSpecifierRE.sub("", errorMessage);
    } while (true);

    return mlir::success();
  }

  llvm::StringRef getErrorMessage(mlir::stablehlo::CustomCallOp op) const {
    return op->getAttr(errorMessageAttrName)
        .cast<mlir::StringAttr>()
        .getValue();
  }

  std::string formatErrorMessage(
      llvm::StringRef errorMessage,
      const mlir::SmallVector<int64_t> &errorMessageInputs) const {
    int nrErrorMessageInputs = errorMessageInputs.size();
    auto errorMessageFormat = errorMessage.data();
    switch (nrErrorMessageInputs) {
      case 0:
        return errorMessageFormat;
      case 1:
        return llvm::formatv(errorMessageFormat, errorMessageInputs[0]);
      case 2:
        return llvm::formatv(errorMessageFormat, errorMessageInputs[0],
                             errorMessageInputs[1]);
      case 3:
        return llvm::formatv(errorMessageFormat, errorMessageInputs[0],
                             errorMessageInputs[1], errorMessageInputs[2]);
      case 4:
        return llvm::formatv(errorMessageFormat, errorMessageInputs[0],
                             errorMessageInputs[1], errorMessageInputs[2],
                             errorMessageInputs[3]);
      default:
        return errorMessageFormat;
    }
  }

  mlir::StringRef getArgument() const override {
    return "check-shape-assertions";
  }

  mlir::StringRef getDescription() const override {
    return "Check stablehlo.custom_call @shape_assertion ops.";
  }

  Option<bool> enable_shape_assertions{
      *this, "enable-shape-assertions",
      llvm::cl::desc("Whether shape assertions may generate errors."),
      llvm::cl::init(true)};
};

}  // namespace

absl::Status RefinePolymorphicShapes(mlir::ModuleOp module,
                                     bool enable_shape_assertions) {
  mlir::MLIRContext *context = module->getContext();
  if (VLOG_IS_ON(3)) context->disableMultithreading();

  // Verify the module before running passes on it.
  // If the module doesn't pass verification, all sorts of weirdness might
  // happen if we run the pass manager.
  mlir::BaseScopedDiagnosticHandler diag_handler(context);

  if (mlir::failed(mlir::verify(module))) {
    return absl::InvalidArgumentError(
        absl::StrCat("Module verification failed: ",
                     diag_handler.ConsumeStatus().ToString()));
  }

  mlir::PassManager pm(context);
  if (VLOG_IS_ON(3)) {
    auto print_before = [](mlir::Pass *, mlir::Operation *) { return true; };
    auto print_after = [](mlir::Pass *, mlir::Operation *) { return true; };
    pm.enableIRPrinting(print_before, print_after, /*printModuleScope=*/true,
                        /*printAfterOnlyOnChange=*/true);
  }

  // TODO(necula): we should not need the inliner.
  pm.addPass(mlir::createInlinerPass());
  pm.addPass(mlir::createCSEPass());
  pm.addPass(mlir::stablehlo::createStablehloRefineShapesPass());
  pm.addNestedPass<mlir::func::FuncOp>(
      mlir::stablehlo::createStablehloCanonicalizeDynamismPass());
  pm.addNestedPass<mlir::func::FuncOp>(
      std::make_unique<CheckShapeAssertionsPass>(enable_shape_assertions));
  if (!mlir::succeeded(pm.run(module))) {
    return absl::InvalidArgumentError(
        absl::StrCat("Module shape refinement failed: ",
                     diag_handler.ConsumeStatus().ToString()));
  }
  return ValidateStaticShapes(module);
}

absl::Status RefinePolymorphicShapes(llvm::StringRef module_str,
                                     llvm::raw_ostream &os,
                                     bool enable_shape_assertions) {
  mlir::MLIRContext context;
  if (VLOG_IS_ON(3)) context.disableMultithreading();
  context.loadDialect<mlir::func::FuncDialect>();
  context.loadDialect<mlir::stablehlo::StablehloDialect>();
  context.loadDialect<mlir::chlo::ChloDialect>();

  mlir::DialectRegistry registry;
  mlir::func::registerAllExtensions(registry);
  context.appendDialectRegistry(registry);

  mlir::OwningOpRef<mlir::ModuleOp> module =
      mlir::parseSourceString<mlir::ModuleOp>(
          llvm::StringRef(module_str.data(), module_str.size()), &context);
  if (!module) {
    return absl::InvalidArgumentError("Cannot parse module.");
  }
  TF_RETURN_IF_ERROR(RefinePolymorphicShapes(*module, enable_shape_assertions));
  if (mlir::failed(mlir::writeBytecodeToFile(*module, os))) {
    return absl::InternalError("Cannot serialize module.");
  }

  return absl::OkStatus();
}

absl::Status ValidateStaticShapes(mlir::ModuleOp module) {
  mlir::BaseScopedDiagnosticHandler diag_handler(module->getContext());
  bool moduleHasDynamicShapes = false;
  bool moduleHasShapeAssertions = false;

  module->walk([&](mlir::Operation *op) {
    // It's sufficient to only check results because operands either come from
    // results or from block arguments which are checked below.
    auto hasDynamicShape = [](mlir::Value value) {
      auto shaped_type = value.getType().dyn_cast<mlir::ShapedType>();
      return shaped_type ? !shaped_type.hasStaticShape() : false;
    };
    bool opHasDynamicShapes = false;
    opHasDynamicShapes |= llvm::any_of(op->getResults(), hasDynamicShape);
    for (mlir::Region &region : op->getRegions()) {
      opHasDynamicShapes |=
          llvm::any_of(region.getArguments(), hasDynamicShape);
    }
    if (opHasDynamicShapes) {
      moduleHasDynamicShapes = true;
      op->emitOpError() << "has dynamic shapes";
    }

    auto customCall = mlir::dyn_cast<mlir::stablehlo::CustomCallOp>(op);
    if (customCall &&
        customCall.getCallTargetName().equals(shapeAssertionName)) {
      moduleHasShapeAssertions = true;
      op->emitOpError() << "has residual shape assertions";
    }
  });

  if (moduleHasDynamicShapes) {
    return absl::InvalidArgumentError(
        absl::StrCat("Module has dynamic shapes: ",
                     diag_handler.ConsumeStatus().ToString()));
  }
  if (moduleHasShapeAssertions) {
    return absl::InvalidArgumentError(
        absl::StrCat("Module has residual shape assertions: ",
                     diag_handler.ConsumeStatus().ToString()));
  }
  return absl::OkStatus();
}

}  // namespace xla
