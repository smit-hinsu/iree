// RUN: iree-opt -split-input-file -verify-diagnostics -iree-flow-dispatch-linalg-on-tensors-pass -canonicalize -cse %s | IreeFileCheck %s

func @tensor(%arg0 : tensor<?x?xf32>, %arg1 : tensor<?x?xf32>,
             %arg2 : tensor<?x?xf32>) -> tensor<?x?xf32> {
  %1 = linalg.matmul ins(%arg0, %arg1 : tensor<?x?xf32>, tensor<?x?xf32>)
    outs(%arg2 : tensor<?x?xf32>) -> tensor<?x?xf32>
  return %1 : tensor<?x?xf32>
}
//      CHECK: func @tensor
// CHECK-SAME:   %[[ARG0:[a-zA-Z0-9_]+]]: tensor<?x?xf32>
// CHECK-SAME:   %[[ARG1:[a-zA-Z0-9_]+]]: tensor<?x?xf32>
// CHECK-SAME:   %[[ARG2:[a-zA-Z0-9_]+]]: tensor<?x?xf32>
//      CHECK:   flow.dispatch.workgroups
// CHECK-SAME:     (%[[ARG0]], %[[ARG1]], %[[ARG2]])
// CHECK-SAME:     %[[ARG3:[a-zA-Z0-9_]+]] : !flow.dispatch.input<?x?xf32>
// CHECK-SAME:     %[[ARG4:[a-zA-Z0-9_]+]] : !flow.dispatch.input<?x?xf32>
// CHECK-SAME:     %[[ARG5:[a-zA-Z0-9_]+]] : !flow.dispatch.input<?x?xf32>
// CHECK-SAME:     %[[ARG6:[a-zA-Z0-9_]+]] : !flow.dispatch.output<?x?xf32>
//  CHECK-DAG:     %[[C0:.+]] = constant 0 : index
//  CHECK-DAG:     %[[WGSIZE_X:.+]] = flow.dispatch.workgroup.size[0]
//  CHECK-DAG:     %[[WGSIZE_Y:.+]] = flow.dispatch.workgroup.size[1]
//  CHECK-DAG:     %[[WGID_X:.+]] = flow.dispatch.workgroup.id[0]
//  CHECK-DAG:     %[[WGID_Y:.+]] = flow.dispatch.workgroup.id[1]
//  CHECK-DAG:     %[[WGCOUNT_X:.+]] = flow.dispatch.workgroup.count[0]
//  CHECK-DAG:     %[[WGCOUNT_Y:.+]] = flow.dispatch.workgroup.count[1]
//      CHECK:     %[[OFFSET_Y:.+]] = muli %[[WGSIZE_Y]], %[[WGID_Y]]
//      CHECK:     %[[STEP_Y:.+]] = muli %[[WGSIZE_Y]], %[[WGCOUNT_Y]]
//      CHECK:     scf.for %[[ARG7:.+]] = %[[OFFSET_Y]]
// CHECK-SAME:       to %{{.+}} step %[[STEP_Y]]
//      CHECK:       %[[OFFSET_X:.+]] = muli %[[WGSIZE_X]], %[[WGID_X]]
//      CHECK:       %[[STEP_X:.+]] = muli %[[WGSIZE_X]], %[[WGCOUNT_X]]
//      CHECK:       scf.for %[[ARG8:.+]] = %[[OFFSET_X]]
// CHECK-SAME:         to %{{.+}} step %[[STEP_X]]
//      CHECK:         %[[LHS:.+]] = flow.dispatch.input.load %[[ARG3]]
// CHECK-SAME:           offsets = [%[[ARG7]], %[[C0]]]
//      CHECK:         %[[RHS:.+]] = flow.dispatch.input.load %[[ARG4]]
// CHECK-SAME:           offsets = [%[[C0]], %[[ARG8]]]
//      CHECK:         %[[INIT:.+]] = flow.dispatch.input.load %[[ARG5]]
// CHECK-SAME:           offsets = [%[[ARG7]], %[[ARG8]]]
//      CHECK:         %[[RESULT:.+]] = linalg.matmul
// CHECK-SAME:           ins(%[[LHS]], %[[RHS]] : tensor<?x?xf32>, tensor<?x?xf32>)
// CHECK-SAME:           outs(%[[INIT]] : tensor<?x?xf32>)
//      CHECK:         flow.dispatch.output.store %[[RESULT]], %[[ARG6]]
// CHECK-SAME:           offsets = [%[[ARG7]], %[[ARG8]]]

// -----

func @generic_op(%A: tensor<?x?xf32>, %B: tensor<?xf32>) -> tensor<?x?xf32> {
  %c0 = constant 0 : index
  %c1 = constant 1 : index
  %d0 = dim %A, %c0 : tensor<?x?xf32>
  %d1 = dim %A, %c1 : tensor<?x?xf32>
  %0 = linalg.init_tensor [%d0, %d1] : tensor<?x?xf32>
  %1 = linalg.generic {
    indexing_maps = [affine_map<(d0, d1) -> (d0, d1)>,
                     affine_map<(d0, d1) -> (d1)>,
                     affine_map<(d0, d1) -> (d0, d1)>],
    iterator_types = ["parallel", "parallel"]}
    ins (%A, %B: tensor<?x?xf32>, tensor<?xf32>)
    outs (%0 : tensor<?x?xf32>) {
      ^bb0(%arg0 : f32, %arg1 : f32, %arg2 : f32):
        %2 = addf %arg0, %arg1 : f32
        linalg.yield %2 : f32
    } -> tensor<?x?xf32>
  return %1 : tensor<?x?xf32>
}
//      CHECK: #[[MAP1:.+]] = affine_map<(d0, d1, d2) -> (d2, d0 - d1)>
//      CHECK: func @generic_op
//      CHECK:   flow.dispatch.workgroups
// CHECK-SAME:     %[[ARG2:[a-zA-Z0-9_]+]] : !flow.dispatch.input<?x?xf32>
// CHECK-SAME:     %[[ARG3:[a-zA-Z0-9_]+]] : !flow.dispatch.input<?xf32>
// CHECK-SAME:     %[[ARG4:[a-zA-Z0-9_]+]] : index
// CHECK-SAME:     %[[ARG5:[a-zA-Z0-9_]+]] : index
// CHECK-SAME:     %[[ARG6:[a-zA-Z0-9_]+]] : !flow.dispatch.output<?x?xf32>
//  CHECK-DAG:     %[[WG_SIZE_X:.+]] = flow.dispatch.workgroup.size[0]
//  CHECK-DAG:     %[[WG_SIZE_Y:.+]] = flow.dispatch.workgroup.size[1]
//      CHECK:     scf.for %[[IV0:[a-zA-Z0-9_]+]]
//      CHECK:       scf.for %[[IV1:.[a-zA-Z0-9_]+]]
//      CHECK:       %[[V1:.+]] = flow.dispatch.input.load %[[ARG2]]
//      CHECK:       %[[V2:.+]] = flow.dispatch.input.load %[[ARG3]]
//      CHECK:       %[[D0:.+]] = affine.min #[[MAP1]](%[[ARG4]], %[[IV0]], %[[WG_SIZE_Y]])
//      CHECK:       %[[D1:.+]] = affine.min #[[MAP1]](%[[ARG5]], %[[IV1]], %[[WG_SIZE_X]])
//      CHECK:       %[[INIT:.+]] = linalg.init_tensor [%[[D0]], %[[D1]]]
//      CHECK:       %[[RESULT:.+]] = linalg.generic
// CHECK-SAME:         ins(%[[V1]], %[[V2]] : tensor<?x?xf32>, tensor<?xf32>)
// CHECK-SAME:         outs(%[[INIT]] : tensor<?x?xf32>)
//      CHECK:         flow.dispatch.output.store %[[RESULT]], %[[ARG6]], offsets = [%[[IV0]], %[[IV1]]], sizes = [%[[D0]], %[[D1]]]

// -----

func @fuse_fill_with_producer(%A : tensor<?x?xf32>, %B : tensor<?x?xf32>) -> tensor<?x?xf32> {
  %zero = constant 0.0 : f32
  %c0 = constant 0 : index
  %c1 = constant 1 : index
  %M = dim %A, %c0 : tensor<?x?xf32>
  %N = dim %B, %c1 : tensor<?x?xf32>
  %0 = linalg.init_tensor [%M, %N] : tensor<?x?xf32>
  %1 = linalg.fill(%0, %zero) : tensor<?x?xf32>, f32 -> tensor<?x?xf32>
  %2 = linalg.matmul ins(%A, %B : tensor<?x?xf32>, tensor<?x?xf32>)
    outs(%1 : tensor<?x?xf32>) -> tensor<?x?xf32>
  return %2 : tensor<?x?xf32>
}
//       CHECK:   func @fuse_fill_with_producer
//  CHECK-SAME:     %[[ARG0:[a-zA-Z0-9_]+]]: tensor<?x?xf32>
//  CHECK-SAME:     %[[ARG1:[a-zA-Z0-9_]+]]: tensor<?x?xf32>
//   CHECK-DAG:     %[[C0:.+]] = constant 0 : index
//   CHECK-DAG:     %[[C1:.+]] = constant 1 : index
//       CHECK:     %[[M:.+]] = dim %[[ARG0]], %[[C0]]
//       CHECK:     %[[N:.+]] = dim %[[ARG1]], %[[C1]]
//       CHECK:     flow.dispatch.workgroups[%[[N]], %[[M]], %[[C1]]]
//  CHECK-SAME:       (%[[M]], %[[N]], %[[ARG0]], %[[ARG1]])
//  CHECK-SAME:       (%[[ARG2:[a-zA-Z0-9_]+]] : index
//  CHECK-SAME:        %[[ARG3:[a-zA-Z0-9_]+]] : index
//  CHECK-SAME:        %[[ARG4:[a-zA-Z0-9_]+]] : !flow.dispatch.input<?x?xf32>
//  CHECK-SAME:        %[[ARG5:[a-zA-Z0-9_]+]] : !flow.dispatch.input<?x?xf32>
//  CHECK-SAME:        %[[ARG6:[a-zA-Z0-9_]+]] : !flow.dispatch.output<?x?xf32>) {
//       CHECK:        %[[ZERO:.+]] = constant 0.000000e+00 : f32
//       CHECK:        scf.for
//       CHECK:          scf.for
//   CHECK-DAG:            %[[LHS_TILE:.+]] = flow.dispatch.input.load %[[ARG4]]
//   CHECK-DAG:            %[[RHS_TILE:.+]] = flow.dispatch.input.load %[[ARG5]]
//   CHECK-DAG:            %[[INIT_TILE:.+]] = linalg.init_tensor
//       CHECK:            %[[FILL_TILE:.+]] = linalg.fill(%[[INIT_TILE]], %[[ZERO]])
//       CHECK:            %[[RESULT_TILE:.+]] = linalg.matmul
//  CHECK-SAME:              ins(%[[LHS_TILE]], %[[RHS_TILE]] : tensor<?x?xf32>, tensor<?x?xf32>)
//  CHECK-SAME:              outs(%[[FILL_TILE]] : tensor<?x?xf32>)
//       CHECK:            flow.dispatch.output.store %[[RESULT_TILE]], %[[ARG6]]
//       CHECK:          flow.return
//       CHECK:        }

// -----

func @two_dispatches(%A : tensor<?x?xf32>, %B : tensor<?x?xf32>) -> tensor<?x?xf32> {
  %zero = constant 0.0 : f32
  %one = constant 1.0 : f32
  %c0 = constant 0 : index
  %c1 = constant 1 : index
  %M = dim %A, %c0 : tensor<?x?xf32>
  %N = dim %B, %c1 : tensor<?x?xf32>
  %K = dim %A, %c1 : tensor<?x?xf32>
  %0 = linalg.init_tensor [%M, %N] : tensor<?x?xf32>
  %1 = linalg.fill(%0, %zero) : tensor<?x?xf32>, f32 -> tensor<?x?xf32>
  %2 = linalg.init_tensor [%M, %K] : tensor<?x?xf32>
  %3 = linalg.generic
    {indexing_maps = [affine_map<(d0, d1) -> (d0, d1)>,
                      affine_map<(d0, d1) -> (d0, d1)>],
     iterator_types = ["parallel", "parallel"]}
    ins(%A : tensor<?x?xf32>) outs(%2 : tensor<?x?xf32>) {
    ^bb0(%arg0 : f32, %arg1 : f32):
      %4 = addf %arg0, %one : f32
      linalg.yield %4 : f32
    } -> tensor<?x?xf32>
  %4 = linalg.matmul ins(%3, %B : tensor<?x?xf32>, tensor<?x?xf32>)
    outs(%1 : tensor<?x?xf32>) -> tensor<?x?xf32>
  return %4 : tensor<?x?xf32>
}
//      CHECK: func @two_dispatches
//  CHECK-SAME:     %[[ARG0:[a-zA-Z0-9_]+]]: tensor<?x?xf32>
//  CHECK-SAME:     %[[ARG1:[a-zA-Z0-9_]+]]: tensor<?x?xf32>
//   CHECK-DAG:     %[[C0:.+]] = constant 0 : index
//   CHECK-DAG:     %[[C1:.+]] = constant 1 : index
//   CHECK-DAG:     %[[M:.+]] = dim %[[ARG0]], %[[C0]]
//   CHECK-DAG:     %[[N:.+]] = dim %[[ARG1]], %[[C1]]
//   CHECK-DAG:     %[[K:.+]] = dim %[[ARG0]], %[[C1]]
//       CHECK:     %[[RESULT1:.+]] = flow.dispatch.workgroups[%[[K]], %[[M]], %[[C1]]]
//  CHECK-SAME:       (%[[ARG0]], %[[M]], %[[K]])
//  CHECK-SAME:       (%[[ARG2:[a-zA-Z0-9_]+]] : !flow.dispatch.input<?x?xf32>
//  CHECK-SAME:        %[[ARG3:[a-zA-Z0-9_]+]] : index
//  CHECK-SAME:        %[[ARG4:[a-zA-Z0-9_]+]] : index
//  CHECK-SAME:        %[[ARG5:[a-zA-Z0-9_]+]] : !flow.dispatch.output<?x?xf32>) {
//       CHECK:          %[[ONE:.+]] = constant 1.0
//       CHECK:          scf.for
//       CHECK:            scf.for
//   CHECK-DAG:              %[[INPUT_TILE:.+]] = flow.dispatch.input.load %[[ARG2]]
//       CHECK:              %[[INIT_TILE:.+]] = linalg.init_tensor
//       CHECK:              %[[RESULT_TILE:.+]] = linalg.generic
//  CHECK-SAME:                ins(%[[INPUT_TILE]] : tensor<?x?xf32>)
//  CHECK-SAME:                outs(%[[INIT_TILE]] : tensor<?x?xf32>)
//       CHECK:              flow.dispatch.output.store %[[RESULT_TILE]], %[[ARG5]]
//       CHECK:          flow.return
//       CHECK:        }
//       CHECK:     flow.dispatch.workgroups[%[[N]], %[[M]], %[[C1]]]
//       CHECK:       %[[ZERO:.+]] = constant 0.0
//       CHECK:       scf.for
//       CHECK:         scf.for
//       CHECK:            %[[INIT_TILE_2:.+]] = linalg.init_tensor
//       CHECK:            %[[FILL_TILE:.+]] = linalg.fill(%[[INIT_TILE_2]], %[[ZERO]])
//       CHECK:            linalg.matmul
//       CHECK:              outs(%[[FILL_TILE]] : tensor<?x?xf32>)

// The following CHECK* sems to hit a segfault with FileCheck. For now using a simpler check.
//  NOCHECK-SAME:       (%[[M]], %[[N]], %[[ARG0]], %[[ARG1]], %[[RESULT1]])
//  NOCHECK-SAME:       (%[[ARG2:[a-zA-Z0-9_]+]] : index
//  NOCHECK-SAME:        %[[ARG3:[a-zA-Z0-9_]+]] : index
//  NOCHECK-SAME:        %[[ARG4:[a-zA-Z0-9_]+]] : !flow.dispatch.input<?x?xf32>
//  NOCHECK-SAME:        %[[ARG5:[a-zA-Z0-9_]+]] : !flow.dispatch.input<?x?xf32>
//  NOCHECK-SAME:        %[[ARG6:[a-zA-Z0-9_]+]] : !flow.dispatch.input<?x?xf32>
//  NOCHECK-SAME:        %[[ARG7:[a-zA-Z0-9_]+]] : !flow.dispatch.output<?x?xf32>) {
//       NOCHECK:          %[[ZERO:.+]] = constant 0.0
//       NOCHECK:          scf.for
//       NOCHECK:            scf.for
//   NOCHECK-DAG:              %[[LHS_TILE_2:.+]] = flow.dispatch.input.load %[[ARG6]]
//   NOCHECK-DAG:              %[[RHS_TILE_2:.+]] = flow.dispatch.input.load %[[ARG5]]
//   NOCHECK-DAG:              %[[INIT_TILE_2:.+]] = linalg.init_tensor
//       NOCHECK:              %[[FILL_TILE:.+]] = linalg.fill(%[[INIT_TILE]], %[[ZERO]])
//       NOCHECK:              %[[RESULT_TILE_2:.++]]] = linalg.matmul
//  NOCHECK-SAME:                ins(%[[LHS_TILE_2]], %[[RHS_TILE_2]] : tensor<?x?xf32>, tensor<?x?xf32>)
//       NOCHECK:                outs(%[[FILL_TILE_2]] : tensor<?x?xf32>)
//       NOCHECK:              flow.dispatch.output.store %[[RESULT_TILE_2]], %[[ARG7]]
//       NOCHECK:          flow.return
//       NOCHECK:        }

// -----

func @dot_general_lower() attributes {iree.module.export} {
  %cst = constant dense<[[[3.000000e-01, 5.000000e-01]]]> : tensor<1x1x2xf32>
  %cst_0 = constant dense<[[1.000000e-01, 2.000000e-01, 3.000000e-01], [4.000000e-01, 5.000000e-01, 6.000000e-01]]> : tensor<2x3xf32>
  %cst_1 = constant dense<[[2.300000e-01, 3.100000e-01, 3.900000e-01]]> : tensor<1x3xf32>
  %cst_2 = constant 0.000000e+00 : f32
  %0 = iree.do_not_optimize(%cst) : tensor<1x1x2xf32>
  %1 = iree.do_not_optimize(%cst_0) : tensor<2x3xf32>
  %2 = linalg.tensor_reshape %0 [affine_map<(d0, d1, d2) -> (d0, d1)>, affine_map<(d0, d1, d2) -> (d2)>] : tensor<1x1x2xf32> into tensor<1x2xf32>
  %3 = linalg.init_tensor [1, 3] : tensor<1x3xf32>
  %4 = linalg.fill(%3, %cst_2) : tensor<1x3xf32>, f32 -> tensor<1x3xf32>
  %5 = linalg.matmul ins(%2, %1 : tensor<1x2xf32>, tensor<2x3xf32>) outs(%4 : tensor<1x3xf32>) -> tensor<1x3xf32>
  check.expect_almost_eq(%5, %cst_1) : tensor<1x3xf32>
  return
}
// CHECK-LABEL: func @dot_general_lower
//       CHECK:   flow.dispatch.workgroups[%{{.+}}, %{{.+}}, %{{.+}}]
//  CHECK-SAME:   %[[ARG0:[a-zA-Z0-9_]+]] : !flow.dispatch.input<1x1x2xf32>
//  CHECK-SAME:   %[[ARG1:[a-zA-Z0-9_]+]] : !flow.dispatch.input<2x3xf32>
//  CHECK-SAME:   %[[ARG2:[a-zA-Z0-9_]+]] : !flow.dispatch.output<1x3xf32>
//   CHECK-DAG:   %[[ZERO:.+]] = constant 0.0
//       CHECK:   %[[LOAD:.+]] = flow.dispatch.input.load %[[ARG0]]
//       CHECK:   %[[RESHAPE:.+]] = linalg.tensor_reshape %[[LOAD]]
//       CHECK:   scf.for
//       CHECK:     scf.for
//   CHECK-DAG:       %[[LHS:.+]] = subtensor %[[RESHAPE]]
//   CHECK-DAG:       %[[RHS:.+]] =  flow.dispatch.input.load %[[ARG1]]
//       CHECK:       %[[INIT:.+]] = linalg.init
//       CHECK:       %[[FILL:.+]] = linalg.fill(%[[INIT]], %[[ZERO]])
//       CHECK:       %[[RESULT:.+]] = linalg.matmul
//  CHECK-SAME:         ins(%[[LHS]], %[[RHS]] : tensor<?x2xf32>, tensor<2x?xf32>)
//  CHECK-SAME:         outs(%[[FILL]] : tensor<?x?xf32>)
//       CHECK:       flow.dispatch.output.store %[[RESULT]], %[[ARG2]]