﻿//
// Copyright (c) 2019-2020 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "convolution_kernel_b_fs_zyx_fsv16.h"
#include "kernel_selector_utils.h"
#include <algorithm>
#include <string>

namespace kernel_selector {

static const size_t sub_group_size = 16;
static const size_t feature_block_size = 16;

FusedOpsConfiguration GenerateFusedOpsConfiguration_f16(size_t conf_id, std::string input_name, Datatype dt,
                                                        bool is_vector) {
    std::vector<std::string> idx_order;
    std::string suffix = (is_vector ? "_VEC" : "_SCALAR") + std::to_string(conf_id);
    std::string input_var_name = input_name + std::to_string(conf_id) + (is_vector ? "" : "[i]");
    size_t vec_size = is_vector ? 8 : 1;
    if (is_vector)
        idx_order = {"(mb)", "(oc*OC_BLOCK + g*OC)", "od", "oh", "(ow + " + std::to_string(conf_id * 8) + ")"};
    else
        idx_order = {"(mb)", "(oc*OC_BLOCK + g*OC + local_id)", "od", "oh", "(ow + " + std::to_string(conf_id * 8) + " + i)"};

    return { suffix,
             idx_order,
             input_var_name,
             dt,
             vec_size,
             is_vector ? FusedOpsConfiguration::LoadType::LT_ALIGNED_READ : FusedOpsConfiguration::LoadType::LT_UNALIGNED,
             FusedOpsConfiguration::BoundaryCheck::ENABLED,
             FusedOpsConfiguration::IndexType::TENSOR_COORD,
             Tensor::DataChannelName::X };
}

FusedOpsConfiguration GenerateFusedOpsConfiguration_bsv16_fsv16(size_t conf_id, std::string input_name, Datatype dt,
                                                                size_t dims) {
    std::vector<std::string> idx_order;
    if (dims == 5)
        idx_order = {"(mb + " + std::to_string(conf_id * 8) + ")", "(oc*16)", "od", "oh", "ow"};
    else
        idx_order = {"(mb + " + std::to_string(conf_id * 8) + ")", "(oc*16)", "oh", "ow"};

    return { "_VEC" + std::to_string(conf_id),
             idx_order,
             input_name + std::to_string(conf_id),
             dt,
             8,
             FusedOpsConfiguration::LoadType::LT_ALIGNED_READ,
             FusedOpsConfiguration::BoundaryCheck::ENABLED,
             FusedOpsConfiguration::IndexType::TENSOR_COORD,
             Tensor::DataChannelName::BATCH };
}

ParamsKey ConvolutionKernel_b_fs_zyx_fsv16::GetSupportedKey() const {
    ParamsKey k;
    k.EnableInputDataType(Datatype::F32);
    k.EnableOutputDataType(Datatype::F32);
    k.EnableInputDataType(Datatype::F16);
    k.EnableOutputDataType(Datatype::F16);
    k.EnableInputWeightsType(WeightsType::F32);
    k.EnableInputWeightsType(WeightsType::F16);
    k.EnableInputLayout(DataLayout::bfzyx);
    k.EnableInputLayout(DataLayout::b_fs_zyx_fsv16);
    k.EnableOutputLayout(DataLayout::b_fs_zyx_fsv16);
    k.EnableInputLayout(DataLayout::bs_fs_zyx_bsv16_fsv16);
    k.EnableOutputLayout(DataLayout::bs_fs_zyx_bsv16_fsv16);
    k.EnableInputLayout(DataLayout::bs_fs_yx_bsv16_fsv16);
    k.EnableOutputLayout(DataLayout::bs_fs_yx_bsv16_fsv16);
    k.EnableTensorOffset();
    k.EnableTensorPitches();
    k.EnableBiasPerFeature();
    k.EnableNonBiasTerm();
    k.EnableSplitSupport();
    k.EnableBatching();
    k.EnableSubGroup();
    k.EnableSubGroupShort();
    k.EnableGroupedConvolution();
    return k;
}

ConvolutionKernelBase::DispatchData ConvolutionKernel_b_fs_zyx_fsv16::SetDefault(const convolution_params& params,
                                                                           int autoTuneIndex) const {
    DispatchData kd = ConvolutionKernelBase::SetDefault(params, autoTuneIndex);

    const auto& out = params.output;
    const auto& input = params.inputs[0];

    auto x = out.X().v;
    auto y = out.Y().v;
    auto z = out.Z().v;
    auto f = out.Feature().v;
    auto b = out.Batch().v;
    auto g = params.groups;

    const bool is_1stconv = input.Feature().v == 3 && input.GetLayout() == DataLayout::bfzyx;
    const bool ver_16mb16c = !is_1stconv &&
        ((out.GetDType() == Datatype::F16 && b % 32 == 0) ||
        (out.GetDType() == Datatype::F32 && b % 16 == 0));

    if (is_1stconv) {
        auto oh_block = 1;
        auto ow_block = 8;
        while (ow_block > 1) {
            if (params.stride.x * ow_block + params.weights.X().v * params.dilation.x > 32)
                ow_block--;
            else
                break;
        }
        kd.cldnnStyle.blockWidth = ow_block;
        if (out.GetDType() == Datatype::F16) {
            kd.lws0 = sub_group_size;
            kd.lws1 = 1;
            kd.lws2 = 1;

            kd.gws0 = (f / 2);
            kd.gws1 = CeilDiv(y, oh_block) * CeilDiv(x, ow_block) * z;
            kd.gws2 = b % 2 == 0 ? b / 2 : b;  // unroll mb by 2
        } else {
            kd.lws0 = sub_group_size;
            kd.lws1 = 1;
            kd.lws2 = 1;

            auto ocb = (f % 32 == 0) ? 32 : 16;
            kd.gws0 = 16;
            kd.gws1 = CeilDiv(y, oh_block) * CeilDiv(x, ow_block) * z;
            kd.gws2 = b * f / ocb;
        }
    } else if (ver_16mb16c) {
        f = (g > 1) ? f/g : Align(f, 16);
        kd.lws0 = sub_group_size;
        kd.lws1 = 1;
        kd.lws2 = 1;

        kd.gws0 = f;
        kd.gws1 = x * y * z;
        kd.gws2 = (out.GetDType() == Datatype::F16) ? b / 32 : b / 16;

        kd.cldnnStyle.blockWidth = 1;
    } else {
        auto oh_block = 1;
        f = Align(f / g, 16);

        auto div = 16;
        while (div > 1) {
            if (x % div == 0)
                break;
            div--;
        }
        auto ow_block = std::max(8, div);

        auto ocb = 128;
        while (ocb > 16) {
            if (f % ocb == 0)
                break;
            else
                ocb /= 2;
        }

        kd.cldnnStyle.blockWidth = ow_block;

        kd.gws0 = ocb;
        kd.gws1 = CeilDiv(y, oh_block) * CeilDiv(x, ow_block) * z;
        kd.gws2 = b * (f / ocb) * g;

        kd.lws0 = sub_group_size;
        kd.lws1 = 1;
        kd.lws2 = 1;
    }
    if (b == 1)
        kd.efficiency = FORCE_PRIORITY_2;
    else
        kd.efficiency = FORCE_PRIORITY_7;

    return kd;
}

bool ConvolutionKernel_b_fs_zyx_fsv16::Validate(const Params& p, const optional_params& o) const {
    if (!ConvolutionKernelBase::Validate(p, o) || !CovolutionCheckInput(p, o)) {
        return false;
    }

    const auto& params = static_cast<const convolution_params&>(p);

    const auto& input = params.inputs[0];
    const auto& output = params.output;

    if (output.GetDType() != use_data_type)
        return false;

    if (input.GetLayout() == DataLayout::bfzyx) {
        if (input.Feature().v != 3 || output.Feature().v % feature_block_size != 0)
            return false;
        if (output.GetDType() == Datatype::F16 && (output.Feature().v % 32 != 0))
            return false;
    } else {
        if ((params.groups > 1) && (input.Feature().v / params.groups) % feature_block_size != 0 &&
            (input.Feature().v / params.groups) != 8)
            return false;
    }

    // Check that padding before features doesn't miss-align the blocks
    if (input.Feature().pad.before % feature_block_size != 0 || output.Feature().pad.before % feature_block_size != 0) {
        return false;
    }

    return true;
}

JitConstants ConvolutionKernel_b_fs_zyx_fsv16::GetJitConstants(const convolution_params& params,
                                                         const DispatchData& runInfo) const {
    auto input = params.inputs[0];
    auto output = params.output;
    auto jit = Parent::GetJitConstants(params, runInfo);

    const bool is_1stconv = input.Feature().v == 3 && input.GetLayout() == DataLayout::bfzyx;
    const bool ver_16mb16c = !is_1stconv && ((output.GetDType() == Datatype::F16 && output.Batch().v % 32 == 0) ||
                                             (output.GetDType() == Datatype::F32 && output.Batch().v % 16 == 0));

    if (ver_16mb16c) {
        jit.AddConstant(MakeJitConstant("VER_16MB16C", 1));
    } else {
        jit.AddConstant(MakeJitConstant("VER_8OW16C", 1));
    }
    jit.AddConstant(MakeJitConstant("OC_BLOCK", 16));
    jit.AddConstant(MakeJitConstant("NCHW", 1));

    if (input.GetLayout() == DataLayout::bs_fs_yx_bsv16_fsv16)
        jit.AddConstant(MakeJitConstant("CASE_3D", 0));
    else
        jit.AddConstant(MakeJitConstant("CASE_3D", 1));

    jit.AddConstant(MakeJitConstant("LWS_0", runInfo.lws0));
    jit.AddConstant(MakeJitConstant("LWS_1", runInfo.lws1));
    jit.AddConstant(MakeJitConstant("LWS_2", runInfo.lws2));

    if (is_1stconv) {
        if (output.GetDType() == Datatype::F16) {
            jit.AddConstant(MakeJitConstant("OCB", 1));
        } else {
            jit.AddConstant(MakeJitConstant("OCB",
                (output.Feature().v % 32 == 0) ? 32 : 16));
        }
    } else if (ver_16mb16c) {
        jit.AddConstant(MakeJitConstant("OCB", 1));
    } else {
        jit.AddConstant(MakeJitConstant("OCB", runInfo.gws0));
    }
    jit.AddConstant(MakeJitConstant("SUM_SCALE", 1));

    auto blockWidth = runInfo.cldnnStyle.blockWidth;

    if (ver_16mb16c) {
        jit.AddConstant(MakeJitConstant("MB_BLOCK", 16));
    } else {
        int mb_block;
        if (output.GetDType() == Datatype::F16)
            mb_block = (is_1stconv && output.Batch().v % 32 == 0) ? 16 : 1;
        else
            mb_block = (is_1stconv && output.Batch().v % 16 == 0) ? 16 : 1;

        jit.AddConstant(MakeJitConstant("MB_BLOCK", mb_block));
    }

    if (ver_16mb16c) {
        jit.AddConstant(MakeJitConstant("IC_BLOCK", 16));
    } else {
        auto ic_block = (is_1stconv && output.GetDType() != Datatype::F16) ? 1 : 16;
        jit.AddConstant(MakeJitConstant("IC_BLOCK", ic_block));
    }

    auto input_dt = GetUnitType(params);

    if (ver_16mb16c && !params.fused_ops.empty()) {
        const auto dims_num = DataTensor::ChannelsCount(input.GetLayout());
        if (output.GetDType() != Datatype::F16) {
            FusedOpsConfiguration conf_vec0 = GenerateFusedOpsConfiguration_bsv16_fsv16(0, "blockC0", input_dt, dims_num);
            FusedOpsConfiguration conf_vec1 = GenerateFusedOpsConfiguration_bsv16_fsv16(1, "blockC0", input_dt, dims_num);
            jit.Merge(MakeFusedOpsJitConstants(params, {conf_vec0, conf_vec1}));
        } else {
            FusedOpsConfiguration conf_vec0 = GenerateFusedOpsConfiguration_bsv16_fsv16(0, "C0", input_dt, dims_num);
            FusedOpsConfiguration conf_vec1 = GenerateFusedOpsConfiguration_bsv16_fsv16(1, "C0", input_dt, dims_num);
            FusedOpsConfiguration conf_vec2 = GenerateFusedOpsConfiguration_bsv16_fsv16(2, "C0", input_dt, dims_num);
            FusedOpsConfiguration conf_vec3 = GenerateFusedOpsConfiguration_bsv16_fsv16(3, "C0", input_dt, dims_num);
            jit.Merge(MakeFusedOpsJitConstants(params, {conf_vec0, conf_vec1, conf_vec2, conf_vec3}));
        }
    } else if (!is_1stconv && !params.fused_ops.empty()) {
        FusedOpsConfiguration conf_vec0 = GenerateFusedOpsConfiguration_f16(0, "blockC0", input_dt, true);
        FusedOpsConfiguration conf_vec1 = GenerateFusedOpsConfiguration_f16(1, "blockC0", input_dt, true);
        FusedOpsConfiguration conf_scalar0 = GenerateFusedOpsConfiguration_f16(0, "blockC0", input_dt, false);
        FusedOpsConfiguration conf_scalar1 = GenerateFusedOpsConfiguration_f16(1, "blockC0", input_dt, false);
        jit.Merge(MakeFusedOpsJitConstants(params, {conf_vec0, conf_vec1, conf_scalar0, conf_scalar1}));
    }

    jit.AddConstant(MakeJitConstant("OH_BLOCK", 1));
    jit.AddConstant(MakeJitConstant("OW_BLOCK", blockWidth));
    jit.AddConstant(MakeJitConstant("OW_LAST", (output.X().v / blockWidth) * blockWidth));
    jit.AddConstant(MakeJitConstant("OWB", CeilDiv(output.X().v, blockWidth)));
    jit.AddConstant(MakeJitConstant("OHB", CeilDiv(output.Y().v, 1)));
    jit.AddConstant(MakeJitConstant("G", params.groups));
    jit.AddConstant(MakeJitConstant("DD", params.dilation.z - 1));
    jit.AddConstant(MakeJitConstant("DH", params.dilation.y - 1));
    jit.AddConstant(MakeJitConstant("DW", params.dilation.x - 1));
    jit.AddConstant(MakeJitConstant("SUB_GROUP_SIZE", sub_group_size));
    jit.AddConstant(MakeJitConstant("FWD_DATA", 1));
    jit.AddConstant(MakeJitConstant("IS_DW", "DEPTHWISE_SEPARABLE_OPT"));
    jit.AddConstant(MakeJitConstant("WITH_BIAS", "BIAS_TERM"));

    if (is_1stconv || params.groups > 1) {
        jit.AddConstant(MakeJitConstant("OC", output.Feature().v / params.groups));
        jit.AddConstant(MakeJitConstant("IC", input.Feature().v / params.groups));
    } else {
        jit.AddConstant(MakeJitConstant("OC", Align(output.Feature().v, 16)));
        jit.AddConstant(MakeJitConstant("IC", Align(input.Feature().v, 16)));
    }

    jit.AddConstant(MakeJitConstant("MB", "OUTPUT_BATCH_NUM"));
    jit.AddConstant(MakeJitConstant("OD", "OUTPUT_SIZE_Z"));
    jit.AddConstant(MakeJitConstant("OH", "OUTPUT_SIZE_Y"));
    jit.AddConstant(MakeJitConstant("OW", "OUTPUT_SIZE_X"));

    jit.AddConstant(MakeJitConstant("ID", "INPUT0_SIZE_Z"));
    jit.AddConstant(MakeJitConstant("IH", "INPUT0_SIZE_Y"));
    jit.AddConstant(MakeJitConstant("IW", "INPUT0_SIZE_X"));
    jit.AddConstant(MakeJitConstant("KD", "FILTER_SIZE_Z"));
    jit.AddConstant(MakeJitConstant("KH", "FILTER_SIZE_Y"));
    jit.AddConstant(MakeJitConstant("KW", "(FILTER_SIZE_X)"));
    jit.AddConstant(MakeJitConstant("SD", "STRIDE_SIZE_Z"));
    jit.AddConstant(MakeJitConstant("SH", "STRIDE_SIZE_Y"));
    jit.AddConstant(MakeJitConstant("SW", "STRIDE_SIZE_X"));
    jit.AddConstant(MakeJitConstant("PD", "PADDING_SIZE_Z"));
    jit.AddConstant(MakeJitConstant("PH", "PADDING_SIZE_Y"));
    jit.AddConstant(MakeJitConstant("PW", "PADDING_SIZE_X"));
    jit.AddConstant(MakeJitConstant("PD_R", "PADDING_SIZE_Z"));
    jit.AddConstant(MakeJitConstant("PH_R", "PADDING_SIZE_Y"));
    jit.AddConstant(MakeJitConstant("PW_R", "PADDING_SIZE_X"));

    if (is_1stconv || params.groups > 1) {
        jit.AddConstant(MakeJitConstant("IC_FULL", params.inputs[0].Feature().LogicalDimPadded()));
        jit.AddConstant(MakeJitConstant("OC_FULL", params.output.Feature().LogicalDimPadded()));
    } else {
        jit.AddConstant(MakeJitConstant("IC_FULL", Align(params.inputs[0].Feature().LogicalDimPadded(), 16)));
        jit.AddConstant(MakeJitConstant("OC_FULL", Align(params.output.Feature().LogicalDimPadded(), 16)));
    }

    jit.AddConstant(MakeJitConstant("ID_FULL", params.inputs[0].Z().LogicalDimPadded()));
    jit.AddConstant(MakeJitConstant("IH_FULL", params.inputs[0].Y().LogicalDimPadded()));
    jit.AddConstant(MakeJitConstant("IW_FULL", params.inputs[0].X().LogicalDimPadded()));
    jit.AddConstant(MakeJitConstant("OD_FULL", params.output.Z().LogicalDimPadded()));
    jit.AddConstant(MakeJitConstant("OH_FULL", params.output.Y().LogicalDimPadded()));
    jit.AddConstant(MakeJitConstant("OW_FULL", params.output.X().LogicalDimPadded()));

    if (params.output.Feature().v % feature_block_size != 0) {
        jit.AddConstant(MakeJitConstant("OUTPUT_LEFTOVERS", 1));
        jit.AddConstant(MakeJitConstant("OC_NOTALLIGNED", output.Feature().v));
    }

    return jit;
}

KernelsData ConvolutionKernel_b_fs_zyx_fsv16::GetKernelsData(const Params& params, const optional_params& options) const {
    return GetTunedKernelsDataByIndex(params, options);
}
}  // namespace kernel_selector
