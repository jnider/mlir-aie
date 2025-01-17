//===- lower_event.mlir ----------------------------------------*- MLIR -*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// (c) Copyright 2023 Advanced Micro Devices, Inc.
//
//===----------------------------------------------------------------------===//

// RUN: aie-opt --aie-standard-lowering %s | FileCheck %s

// CHECK: call @llvm.aie.event0()
// CHECK: call @llvm.aie.event1()
module @test {
 AIE.device(xcvc1902) {
  %tile11 = AIE.tile(1, 1)
  %core11 = AIE.core(%tile11) {
    AIE.event(0)
    AIE.event(1)
    AIE.end
  }
 }
}
