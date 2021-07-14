// Copyright 2021 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/compiler/Dialect/Flow/IR/FlowDialect.h"
#include "iree/compiler/Dialect/Flow/IR/FlowOps.h"
#include "iree/compiler/Dialect/LinalgExt/IR/LinalgExtOps.h"
#include "iree/compiler/Dialect/LinalgExt/Transforms/PassDetail.h"
#include "iree/compiler/Dialect/LinalgExt/Transforms/Passes.h"
#include "iree/compiler/Dialect/LinalgExt/Transforms/Transforms.h"
#include "llvm/ADT/TypeSwitch.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Linalg/IR/LinalgOps.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/SCF.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

namespace mlir {
namespace iree_compiler {
namespace linalg_ext {

//===----------------------------------------------------------------------===//
// Utility methods for tiling a linalg_ext operation that implements a
// TiledOpInterface
//===----------------------------------------------------------------------===//

/// Returns failure if the options are unsupported.
static LogicalResult verifySupportedTilingOptions(
    PatternRewriter &rewriter, Operation *op,
    const linalg::LinalgTilingOptions &options) {
  if (!options.interchangeVector.empty()) {
    return rewriter.notifyMatchFailure(op,
                                       "unsupported interchange during tiling");
  }
  if (options.paddingValueComputationFunction) {
    return rewriter.notifyMatchFailure(op, "unsupported tile + pad option");
  }
  if (options.loopType != linalg::LinalgTilingLoopType::Loops) {
    return rewriter.notifyMatchFailure(op,
                                       "only tiling with scf.for is supported");
  }
  if (options.distribution) {
    if (llvm::any_of(options.distribution->distributionMethod,
                     [](linalg::DistributionMethod method) {
                       return method != linalg::DistributionMethod::Cyclic;
                     })) {
      return rewriter.notifyMatchFailure(op,
                                         "only cyclic distibution is allowed");
    }
  }
  return success();
}

/// Converts a `Value` to an `OpFoldRedult` by extracting the constant value if
/// the value is defined by a constant op.
static OpFoldResult getOpFoldResult(Value value) {
  IntegerAttr::ValueType attr;
  if (matchPattern(value, m_ConstantInt(&attr))) {
    return IntegerAttr::get(value.getType(), attr);
  }
  return value;
}
static SmallVector<OpFoldResult, 4> getOpFoldResult(ArrayRef<Value> values) {
  return llvm::to_vector<4>(llvm::map_range(
      values, [](Value value) { return getOpFoldResult(value); }));
}

/// Converts an `OpFoldResult` to a `Value` by building a constant op if
/// if the `OpFoldResult` is an `IntegerAttr`.
static Value getValue(OpBuilder &builder, Location loc,
                      OpFoldResult valueOrAttr) {
  if (auto attr = valueOrAttr.dyn_cast<Attribute>()) {
    return builder.create<ConstantIndexOp>(loc,
                                           attr.cast<IntegerAttr>().getInt());
  }
  return valueOrAttr.get<Value>();
}

/// Returns true if loop is untiled. Only checks if the value is statically
/// zero. It is assumed that a `Value` defined by a constant op is already
/// converted to an `IntegerAttr` of that value. So here just return true if
/// this is an attribute with a zero value.
static bool isUntiledLoop(OpFoldResult valueOrAttr) {
  auto attr = valueOrAttr.dyn_cast<Attribute>();
  return attr && attr.cast<IntegerAttr>().getValue() == 0;
}

/// Generates the tiled loops and the body by invoking the interface methods of
/// TiledOpInterface.
/// - `outputs` are the operands to use for outputs of the tiled operation.
/// - `tileSizes` are tile sizes specified for all loops of the operation. If a
///   loop is to be untiled it is set to 0.
/// - `iteratorType` is the type of the loop iterator returned by the
///   TiledOpInterface.
/// - `loopBounds` are the bounds of all the loops of the op returned by the
///   TiledOpInterface.
/// - `loopDepth` is the current loop depth being processed.
/// - `offsets` are the `Value`s that represent the position of the tile being
///   operated on. The offsets are computed as the tiled loops are being
///   generated.
/// - `distributionInfo` is the proc_id and nprocs `Value`s to be used for
///   distributed loops. It is a stack, and once an entry at the top of the
///   stack is used for distribution it is popped before processing the inner
///   loops.
static FailureOr<TiledOp> tileLinalgExtOpImpl(
    OpBuilder &builder, TiledOpInterface op, ValueRange outputs,
    MutableArrayRef<OpFoldResult> tileSizes, ArrayRef<StringRef> iteratorTypes,
    ArrayRef<Range> loopBounds, unsigned loopDepth,
    SmallVectorImpl<OpFoldResult> &offsets,
    ArrayRef<linalg::ProcInfo> distributionInfo) {
  Location loc = op.getLoc();
  // If this is the innermost loop, then generated the tiled implementation of
  // the op by invoking the TiledOpInterface methods.
  if (loopDepth == tileSizes.size()) {
    SmallVector<SmallVector<OpFoldResult, 4>> resultOffsets;
    Operation *tiledOp = op.getTiledImplementation(builder, outputs, offsets,
                                                   tileSizes, resultOffsets);
    if (!tiledOp) {
      return static_cast<LogicalResult>(
          op.emitOpError("failed to get tiled implementation"));
    }
    assert(tiledOp->getNumResults() == 0 ||
           (resultOffsets.size() == tiledOp->getNumResults()));
    TiledOp ret;
    ret.op = tiledOp;

    // If the operation has results, then the result of the tiled operation is
    // to be inserted into the `initValues` and returned.
    if (tiledOp->getNumResults()) {
      SmallVector<Value> results;
      results.reserve(tiledOp->getNumResults());
      for (auto en : llvm::enumerate(tiledOp->getResults())) {
        Value result = en.value();
        ArrayRef<OpFoldResult> offsets(resultOffsets[en.index()]);
        auto resultType = result.getType().cast<ShapedType>();
        auto oneAttr = builder.getI64IntegerAttr(1);
        SmallVector<OpFoldResult> strides(resultType.getRank(), oneAttr);
        auto sizes = llvm::to_vector<4>(llvm::map_range(
            llvm::seq<int64_t>(0, resultType.getRank()),
            [&](int64_t dim) { return getDim(builder, loc, result, dim); }));
        Value insert = builder.create<tensor::InsertSliceOp>(
            loc, result, outputs[en.index()], offsets, sizes, strides);
        results.push_back(insert);
      }
      std::swap(ret.results, results);
    }
    return ret;
  }

  // If tile size at this depth is empty, do nothing.
  if (isUntiledLoop(tileSizes[loopDepth])) {
    auto zeroAttr = builder.getI64IntegerAttr(0);
    offsets.push_back(zeroAttr);
    assert(matchPattern(loopBounds[loopDepth].offset, m_Zero()) &&
           "expected loop bounds to have lower bound of zero");
    tileSizes[loopDepth] = getOpFoldResult(loopBounds[loopDepth].size);
    return tileLinalgExtOpImpl(builder, op, outputs, tileSizes, iteratorTypes,
                               loopBounds, loopDepth + 1, offsets,
                               distributionInfo);
  }

  // Generate an scf.for for the current loop depth.
  Value lb = loopBounds[loopDepth].offset;
  Value ub = loopBounds[loopDepth].size;
  if (!matchPattern(loopBounds[loopDepth].stride, m_One())) {
    return static_cast<LogicalResult>(
        op.emitOpError("expected stride to be 1"));
  }
  Value step = getValue(builder, loc, tileSizes[loopDepth]);

  // Update lb, ub and step for cyclic distribution.
  if (!distributionInfo.empty() &&
      iteratorTypes[loopDepth] == getParallelIteratorTypeName()) {
    linalg::updateBoundsForCyclicDistribution(
        builder, loc, distributionInfo.front().procId,
        distributionInfo.front().nprocs, lb, ub, step);
    distributionInfo = distributionInfo.drop_front();
  }
  FailureOr<TiledOp> innerReturnValue;
  bool isBufferTiling = op->getNumResults() == 0;
  ValueRange initValues(isBufferTiling ? ValueRange{} : outputs);
  auto forOp = builder.create<scf::ForOp>(
      loc, lb, ub, step, initValues,
      [&](OpBuilder &b, Location loc, Value iv, ValueRange args) {
        offsets.push_back(iv);
        auto affineMaps = AffineMap::inferFromExprList({ArrayRef<AffineExpr>{
            b.getAffineSymbolExpr(0),
            b.getAffineSymbolExpr(1) - b.getAffineDimExpr(0)}})[0];
        // Similar to linalg tiling, the tile size is the min(tileSizes, ub -
        // iv) to account for cases where tile size does not divide (ub - lb)
        // exactly.
        Value inBoundsTileSize = b.create<AffineMinOp>(
            loc, affineMaps,
            ValueRange{iv, getValue(builder, loc, tileSizes[loopDepth]), ub});
        tileSizes[loopDepth] = getOpFoldResult(inBoundsTileSize);
        // Recursively proceed to generate the tiled loop for the next level.
        innerReturnValue = tileLinalgExtOpImpl(
            b, op, (isBufferTiling ? outputs : args), tileSizes, iteratorTypes,
            loopBounds, loopDepth + 1, offsets, distributionInfo);
        if (failed(innerReturnValue)) return;
        b.create<scf::YieldOp>(loc, innerReturnValue->results);
      });
  if (failed(innerReturnValue)) {
    return innerReturnValue;
  }
  innerReturnValue->loops.insert(innerReturnValue->loops.begin(),
                                 forOp.getOperation());
  innerReturnValue->results = forOp.getResults();
  return innerReturnValue;
}

FailureOr<TiledOp> tileLinalgExtOp(OpBuilder &b, LinalgExtOp op,
                                   const linalg::LinalgTilingOptions &options) {
  TiledOpInterface tilableOp = dyn_cast<TiledOpInterface>(op.getOperation());
  if (!tilableOp) return TiledOp{};

  SmallVector<StringRef> iteratorTypes = tilableOp.getLoopIteratorTypes();
  SmallVector<Value, 4> tileSizesVals =
      options.tileSizeComputationFunction(b, tilableOp.getOperation());
  auto zeroAttr = b.getI64IntegerAttr(0);

  // The actual tile sizes used converts `Value` defined as constant 0, to a
  // zero integer attributes. Currently if the iterator type is not "parallel",
  // the tile size is forced to zero as well.
  auto tileSizes = getOpFoldResult(tileSizesVals);
  tileSizes.resize(iteratorTypes.size(), zeroAttr);
  for (auto en : llvm::enumerate(iteratorTypes)) {
    if (en.value() == getParallelIteratorTypeName()) continue;
    if (!isUntiledLoop(tileSizes[en.index()])) {
      return static_cast<LogicalResult>(op.emitOpError(
          "unimplemented tiling of non-parallel loop iterator type"));
    }
  }

  // Trivial early exit case of tile sizes being zero for all parallel loops.
  if (llvm::all_of(tileSizes, isUntiledLoop)) {
    return TiledOp{op.getOperation(), {}, {}};
  }

  SmallVector<Range> loopBounds = tilableOp.getLoopBounds(b);
  SmallVector<linalg::ProcInfo> distributionInfo;
  // If the tiled loops are distributed, get the proc_id and nprocs for the
  // distributed loops. First collect the parallel loops by iterating over the
  // tileSizes and getting the loops that are distribute, i.e.,
  // - parallel, i.e. iteratorTypes is "parallel"
  // - tiled, i.e. tileSize != 0
  if (options.distribution) {
    SmallVector<Range> distributedLoopRange;
    for (auto i : llvm::seq<unsigned>(0, tileSizes.size())) {
      if (isUntiledLoop(tileSizes[i])) continue;
      if (iteratorTypes[i] != getParallelIteratorTypeName()) continue;
      distributedLoopRange.push_back(loopBounds[i]);
    }
    distributionInfo =
        options.distribution->procInfo(b, op.getLoc(), distributedLoopRange);
  }

  SmallVector<OpFoldResult> offsets;
  return tileLinalgExtOpImpl(b, tilableOp, op.outputs(), tileSizes,
                             iteratorTypes, loopBounds, 0, offsets,
                             distributionInfo);
}

//===----------------------------------------------------------------------===//
// Patterns for tiling LinalgExtOps.
//===----------------------------------------------------------------------===//

namespace {
/// Base pattern for tiling LinalgExtOps.
struct LinalgExtBaseTilingPattern : public RewritePattern {
  LinalgExtBaseTilingPattern(StringRef opName, MLIRContext *context,
                             linalg::LinalgTilingOptions options,
                             linalg::LinalgTransformationFilter filter =
                                 linalg::LinalgTransformationFilter(),
                             PatternBenefit benefit = 1)
      : RewritePattern(opName, benefit, context),
        filter(filter),
        options(options) {}

  LogicalResult matchAndRewriteBase(Operation *op, PatternRewriter &rewriter,
                                    TiledOp &result) const;

 private:
  /// LinalgTransformMarker handles special attribute manipulations.
  linalg::LinalgTransformationFilter filter;
  /// Options to control tiling;
  linalg::LinalgTilingOptions options;
};

template <typename OpTy>
struct LinalgExtTilingPattern : public LinalgExtBaseTilingPattern {
  LinalgExtTilingPattern(MLIRContext *context,
                         linalg::LinalgTilingOptions options,
                         linalg::LinalgTransformationFilter filter =
                             linalg::LinalgTransformationFilter(),
                         PatternBenefit benefit = 1)
      : LinalgExtBaseTilingPattern(OpTy::getOperationName(), context, options,
                                   filter, benefit) {}

  LogicalResult matchAndRewrite(Operation *op,
                                PatternRewriter &rewriter) const override {
    TiledOp tiledOp;
    // Check for failure.
    if (failed(LinalgExtBaseTilingPattern::matchAndRewriteBase(op, rewriter,
                                                               tiledOp))) {
      return failure();
    }
    // Check for do-nothing case.
    if (!tiledOp.op) return failure();
    if (tiledOp.op != op) {
      if (tiledOp.results.empty()) {
        rewriter.eraseOp(op);
      } else {
        rewriter.replaceOp(op, tiledOp.results);
      }
    }
    return success();
  }
};
}  // namespace

LogicalResult LinalgExtBaseTilingPattern::matchAndRewriteBase(
    Operation *op, PatternRewriter &rewriter, TiledOp &result) const {
  auto linalgExtOp = dyn_cast<LinalgExtOp>(op);
  if (!linalgExtOp) return failure();
  if (failed(filter.checkAndNotify(rewriter, op))) return failure();
  if (failed(verifySupportedTilingOptions(rewriter, op, options))) {
    return failure();
  }

  FailureOr<TiledOp> res = tileLinalgExtOp(rewriter, linalgExtOp, options);
  if (failed(res)) return res;
  result = *res;
  if (result.op) {
    filter.replaceLinalgTransformationFilter(rewriter, result.op);
  }
  return success();
}

//===----------------------------------------------------------------------===//
// Test pass for tiling Linalg Ext ops
//===----------------------------------------------------------------------===//

namespace {
struct LinalgExtTilingPass : public LinalgExtTilingBase<LinalgExtTilingPass> {
  void getDependentDialects(DialectRegistry &registry) const override {
    registry
        .insert<AffineDialect, IREE::Flow::FlowDialect, linalg::LinalgDialect,
                memref::MemRefDialect, StandardOpsDialect,
                tensor::TensorDialect, scf::SCFDialect>();
  }

  void runOnOperation() override;
};
}  // namespace

template <typename OpTy>
static Value buildFlowWorkgroupInfoOp(OpBuilder &b, unsigned dim) {
  return b.template create<OpTy>(b.getInsertionPoint()->getLoc(), dim);
}

void LinalgExtTilingPass::runOnOperation() {
  FuncOp funcOp = getOperation();
  MLIRContext *context = funcOp.getContext();
  RewritePatternSet patterns(context);
  patterns.add<LinalgExtTilingPattern<ScatterOp>>(
      context, linalg::LinalgTilingOptions().setTileSizes({10, 20}),
      linalg::LinalgTransformationFilter(
          Identifier::get("tiling_input", context),
          Identifier::get("tiling_output", context)));
  patterns.add<LinalgExtTilingPattern<ScatterOp>>(
      context, linalg::LinalgTilingOptions().setTileSizes(ArrayRef<int64_t>{0}),
      linalg::LinalgTransformationFilter(
          Identifier::get("no_tiling_input", context),
          Identifier::get("no_tiling_output", context)));
  patterns.add<LinalgExtTilingPattern<SortOp>>(
      context, linalg::LinalgTilingOptions().setTileSizes({0, 20}),
      linalg::LinalgTransformationFilter(
          Identifier::get("outer_reduce_input", context),
          Identifier::get("outer_reduce_output", context)));
  patterns.add<LinalgExtTilingPattern<SortOp>>(
      context, linalg::LinalgTilingOptions().setTileSizes({10, 0, 0}),
      linalg::LinalgTransformationFilter(
          Identifier::get("inner_reduce_input", context),
          Identifier::get("inner_reduce_output", context)));

  static linalg::LinalgLoopDistributionOptions workgroupDistributionOptions = {
      [](OpBuilder &builder, Location loc, ArrayRef<Range> parallelLoopRanges) {
        auto numParallelDims = parallelLoopRanges.size();

        SmallVector<linalg::ProcInfo, 3> procInfo(numParallelDims);
        for (size_t dim = 0; dim < numParallelDims; ++dim) {
          procInfo[numParallelDims - dim - 1] = {
              buildFlowWorkgroupInfoOp<IREE::Flow::DispatchWorkgroupIDOp>(
                  builder, dim),
              buildFlowWorkgroupInfoOp<IREE::Flow::DispatchWorkgroupCountOp>(
                  builder, dim)};
        }
        return procInfo;
      },
      {linalg::DistributionMethod::Cyclic, linalg::DistributionMethod::Cyclic,
       linalg::DistributionMethod::Cyclic},
      DenseMap<StringRef,
               std::function<linalg::ProcInfo(OpBuilder &, Location)>>()};

  patterns
      .add<LinalgExtTilingPattern<ScatterOp>, LinalgExtTilingPattern<SortOp>>(
          context,
          linalg::LinalgTilingOptions()
              .setTileSizes(ArrayRef<int64_t>{10, 0, 30})
              .setDistributionOptions(workgroupDistributionOptions),
          linalg::LinalgTransformationFilter(
              Identifier::get("distribute_input", context),
              Identifier::get("distribute_output", context)));

  if (failed(applyPatternsAndFoldGreedily(funcOp, std::move(patterns)))) {
    return signalPassFailure();
  }
}

std::unique_ptr<OperationPass<FuncOp>> createLinalgExtTilingPass() {
  return std::make_unique<LinalgExtTilingPass>();
}

}  // namespace linalg_ext
}  // namespace iree_compiler
}  // namespace mlir