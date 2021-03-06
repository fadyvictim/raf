# Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
# SPDX-License-Identifier: Apache-2.0

"""The extended conv2d operation. Origin: cutlass/tools/library/scripts/conv2d_operation.py"""
import conv2d_operation
from library import *
from library_ext import *


class Conv2dOperationExt(conv2d_operation.Conv2dOperation):
    def core_name(self):
        intermediate_type = ""

        if self.tile_description.math_instruction.opcode_class == OpcodeClass.TensorOp:
            inst_shape = "%d%d%d" % tuple(self.tile_description.math_instruction.instruction_shape)
            if (
                self.tile_description.math_instruction.element_a != self.A.element
                and self.tile_description.math_instruction.element_a != self.accumulator_type()
            ):
                intermediate_type = DataTypeNames[self.tile_description.math_instruction.element_a]
        else:
            inst_shape = ""

        return "%s%s%s%s%s_%s" % (
            ShortDataTypeNames[self.accumulator_type()],
            inst_shape,
            intermediate_type,
            ConvKindNames[self.conv_kind],
            EpilogueFunctorNames[self.epilogue_functor],
            IteratorAlgorithmNames[self.iterator_algorithm],
        )


class EmitConv2dConfigurationLibraryExt(conv2d_operation.EmitConv2dConfigurationLibrary):
    def __init__(self, operation_path, configuration_name):
        super().__init__(operation_path, configuration_name)

        self.configuration_instance = """
  using Operation_${operation_name} = cutlass::conv::device::ImplicitGemmConvolution<
    ${operation_name}>;

  manifest.append(new cutlass::library::Conv2dOperationExt<
    Operation_${operation_name}>(
      "${operation_name}"));

"""

        self.header_template = """
/*
  Generated by conv2d_operation.py - Do not edit.
*/

///////////////////////////////////////////////////////////////////////////////////////////////////

#include "cutlass/cutlass.h"
#include "cutlass/library/library.h"
#include "cutlass/library/manifest.h"
#include "cutlass_ext/library/conv2d_operation_ext.h"

#include "library_internal.h"
#include "conv2d_operation.h"

///////////////////////////////////////////////////////////////////////////////////////////////////
"""


def make_conv2d_operation_ext(op):
    return Conv2dOperationExt(
        op.conv_kind,
        op.iterator_algorithm,
        op.arch,
        op.tile_description,
        op.A,
        op.B,
        op.C,
        op.element_epilogue,
        op.stride_support,
        op.epilogue_functor,
        op.swizzling_functor,
    )
