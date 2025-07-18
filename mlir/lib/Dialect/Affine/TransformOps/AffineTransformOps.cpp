//=== AffineTransformOps.cpp - Implementation of Affine transformation ops ===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/Affine/TransformOps/AffineTransformOps.h"
#include "mlir/Dialect/Affine/Analysis/AffineStructures.h"
#include "mlir/Dialect/Affine/Analysis/Utils.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Affine/IR/AffineValueMap.h"
#include "mlir/Dialect/Affine/LoopUtils.h"
#include "mlir/Dialect/Affine/Transforms/Transforms.h"
#include "mlir/Dialect/Transform/IR/TransformDialect.h"
#include "mlir/Dialect/Transform/Interfaces/TransformInterfaces.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

using namespace mlir;
using namespace mlir::affine;
using namespace mlir::transform;

//===----------------------------------------------------------------------===//
// SimplifyBoundedAffineOpsOp
//===----------------------------------------------------------------------===//

LogicalResult SimplifyBoundedAffineOpsOp::verify() {
  if (getLowerBounds().size() != getBoundedValues().size())
    return emitOpError() << "incorrect number of lower bounds, expected "
                         << getBoundedValues().size() << " but found "
                         << getLowerBounds().size();
  if (getUpperBounds().size() != getBoundedValues().size())
    return emitOpError() << "incorrect number of upper bounds, expected "
                         << getBoundedValues().size() << " but found "
                         << getUpperBounds().size();
  return success();
}

namespace {
/// Simplify affine.min / affine.max ops with the given constraints. They are
/// either rewritten to affine.apply or left unchanged.
template <typename OpTy>
struct SimplifyAffineMinMaxOp : public OpRewritePattern<OpTy> {
  using OpRewritePattern<OpTy>::OpRewritePattern;
  SimplifyAffineMinMaxOp(MLIRContext *ctx,
                         const FlatAffineValueConstraints &constraints,
                         PatternBenefit benefit = 1)
      : OpRewritePattern<OpTy>(ctx, benefit), constraints(constraints) {}

  LogicalResult matchAndRewrite(OpTy op,
                                PatternRewriter &rewriter) const override {
    FailureOr<AffineValueMap> simplified =
        simplifyConstrainedMinMaxOp(op, constraints);
    if (failed(simplified))
      return failure();
    rewriter.replaceOpWithNewOp<AffineApplyOp>(op, simplified->getAffineMap(),
                                               simplified->getOperands());
    return success();
  }

  const FlatAffineValueConstraints &constraints;
};
} // namespace

DiagnosedSilenceableFailure
SimplifyBoundedAffineOpsOp::apply(transform::TransformRewriter &rewriter,
                                  TransformResults &results,
                                  TransformState &state) {
  // Get constraints for bounded values.
  SmallVector<int64_t> lbs;
  SmallVector<int64_t> ubs;
  SmallVector<Value> boundedValues;
  DenseSet<Operation *> boundedOps;
  for (const auto &it : llvm::zip_equal(getBoundedValues(), getLowerBounds(),
                                        getUpperBounds())) {
    Value handle = std::get<0>(it);
    for (Operation *op : state.getPayloadOps(handle)) {
      if (op->getNumResults() != 1 || !op->getResult(0).getType().isIndex()) {
        auto diag =
            emitDefiniteFailure()
            << "expected bounded value handle to point to one or multiple "
               "single-result index-typed ops";
        diag.attachNote(op->getLoc()) << "multiple/non-index result";
        return diag;
      }
      boundedValues.push_back(op->getResult(0));
      boundedOps.insert(op);
      lbs.push_back(std::get<1>(it));
      ubs.push_back(std::get<2>(it));
    }
  }

  // Build constraint set.
  FlatAffineValueConstraints cstr;
  for (const auto &it : llvm::zip(boundedValues, lbs, ubs)) {
    unsigned pos;
    if (!cstr.findVar(std::get<0>(it), &pos))
      pos = cstr.appendSymbolVar(std::get<0>(it));
    cstr.addBound(presburger::BoundType::LB, pos, std::get<1>(it));
    // Note: addBound bounds are inclusive, but specified UB is exclusive.
    cstr.addBound(presburger::BoundType::UB, pos, std::get<2>(it) - 1);
  }

  // Transform all targets.
  SmallVector<Operation *> targets;
  for (Operation *target : state.getPayloadOps(getTarget())) {
    if (!isa<AffineMinOp, AffineMaxOp>(target)) {
      auto diag = emitDefiniteFailure()
                  << "target must be affine.min or affine.max";
      diag.attachNote(target->getLoc()) << "target op";
      return diag;
    }
    if (boundedOps.contains(target)) {
      auto diag = emitDefiniteFailure()
                  << "target op result must not be constrained";
      diag.attachNote(target->getLoc()) << "target/constrained op";
      return diag;
    }
    targets.push_back(target);
  }
  RewritePatternSet patterns(getContext());
  // Canonicalization patterns are needed so that affine.apply ops are composed
  // with the remaining affine.min/max ops.
  AffineMaxOp::getCanonicalizationPatterns(patterns, getContext());
  AffineMinOp::getCanonicalizationPatterns(patterns, getContext());
  patterns.insert<SimplifyAffineMinMaxOp<AffineMinOp>,
                  SimplifyAffineMinMaxOp<AffineMaxOp>>(getContext(), cstr);
  FrozenRewritePatternSet frozenPatterns(std::move(patterns));
  // Apply the simplification pattern to a fixpoint.
  if (failed(applyOpPatternsGreedily(
          targets, frozenPatterns,
          GreedyRewriteConfig()
              .setListener(
                  static_cast<RewriterBase::Listener *>(rewriter.getListener()))
              .setStrictness(GreedyRewriteStrictness::ExistingAndNewOps)))) {
    auto diag = emitDefiniteFailure()
                << "affine.min/max simplification did not converge";
    return diag;
  }
  return DiagnosedSilenceableFailure::success();
}

void SimplifyBoundedAffineOpsOp::getEffects(
    SmallVectorImpl<MemoryEffects::EffectInstance> &effects) {
  consumesHandle(getTargetMutable(), effects);
  for (OpOperand &operand : getBoundedValuesMutable())
    onlyReadsHandle(operand, effects);
  modifiesPayload(effects);
}

//===----------------------------------------------------------------------===//
// SimplifyMinMaxAffineOpsOp
//===----------------------------------------------------------------------===//
DiagnosedSilenceableFailure
SimplifyMinMaxAffineOpsOp::apply(transform::TransformRewriter &rewriter,
                                 TransformResults &results,
                                 TransformState &state) {
  SmallVector<Operation *> targets;
  for (Operation *target : state.getPayloadOps(getTarget())) {
    if (!isa<AffineMinOp, AffineMaxOp>(target)) {
      auto diag = emitDefiniteFailure()
                  << "target must be affine.min or affine.max";
      diag.attachNote(target->getLoc()) << "target op";
      return diag;
    }
    targets.push_back(target);
  }
  bool modified = false;
  if (failed(mlir::affine::simplifyAffineMinMaxOps(rewriter, targets,
                                                   &modified))) {
    return emitDefiniteFailure()
           << "affine.min/max simplification did not converge";
  }
  if (!modified) {
    return emitSilenceableError()
           << "the transform failed to simplify any of the target operations";
  }
  return DiagnosedSilenceableFailure::success();
}

void SimplifyMinMaxAffineOpsOp::getEffects(
    SmallVectorImpl<MemoryEffects::EffectInstance> &effects) {
  consumesHandle(getTargetMutable(), effects);
  modifiesPayload(effects);
}

//===----------------------------------------------------------------------===//
// Transform op registration
//===----------------------------------------------------------------------===//

namespace {
class AffineTransformDialectExtension
    : public transform::TransformDialectExtension<
          AffineTransformDialectExtension> {
public:
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(AffineTransformDialectExtension)

  using Base::Base;

  void init() {
    declareGeneratedDialect<AffineDialect>();

    registerTransformOps<
#define GET_OP_LIST
#include "mlir/Dialect/Affine/TransformOps/AffineTransformOps.cpp.inc"
        >();
  }
};
} // namespace

#define GET_OP_CLASSES
#include "mlir/Dialect/Affine/TransformOps/AffineTransformOps.cpp.inc"

void mlir::affine::registerTransformDialectExtension(
    DialectRegistry &registry) {
  registry.addExtensions<AffineTransformDialectExtension>();
}
