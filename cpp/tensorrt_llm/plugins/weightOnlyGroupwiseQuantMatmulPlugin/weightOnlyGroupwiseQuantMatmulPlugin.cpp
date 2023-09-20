/*
 * SPDX-FileCopyrightText: Copyright (c) 1993-2022 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "tensorrt_llm/plugins/weightOnlyGroupwiseQuantMatmulPlugin/weightOnlyGroupwiseQuantMatmulPlugin.h"

using namespace nvinfer1;
using namespace tensorrt_llm::common;
using namespace tensorrt_llm::kernels::cutlass_kernels;
using nvinfer1::plugin::WeightOnlyGroupwiseQuantMatmulPluginCreator;
using nvinfer1::plugin::WeightOnlyGroupwiseQuantMatmulPlugin;

static const char* WOQ_GROUPWISE_MATMUL_PLUGIN_VERSION{"1"};
static const char* WOQ_GROUPWISE_MATMUL_PLUGIN_NAME{"WeightOnlyGroupwiseQuantMatmul"};
PluginFieldCollection WeightOnlyGroupwiseQuantMatmulPluginCreator::mFC{};
std::vector<PluginField> WeightOnlyGroupwiseQuantMatmulPluginCreator::mPluginAttributes;

WeightOnlyGroupwiseQuantMatmulPlugin::WeightOnlyGroupwiseQuantMatmulPlugin(
    nvinfer1::DataType type, int quant_algo, int group_size)
{
    init(type, quant_algo, group_size);
}

// Parameterized constructor
WeightOnlyGroupwiseQuantMatmulPlugin::WeightOnlyGroupwiseQuantMatmulPlugin(const void* data, size_t length)
{
    const char *d = reinterpret_cast<const char*>(data), *a = d;
    nvinfer1::DataType type;
    int quant_algo = 0;
    int group_size = 0;
    read(d, type);
    read(d, quant_algo);
    read(d, group_size);
    init(type, quant_algo, group_size);
    PLUGIN_ASSERT(d == a + length);
}

void WeightOnlyGroupwiseQuantMatmulPlugin::init(nvinfer1::DataType type, int quant_algo, int group_size)
{
    mType = type;
    mQuantAlgo = quant_algo;
    mGroupSize = group_size;

    // quant_algo = pre_quant_scale * 4 + zero * 2 + bias
    mPreQuantScaleInputIdx = (quant_algo & PRE_SCALE_QUANT) ? 1 : 0;
    mWeightInputIdx = mPreQuantScaleInputIdx + 1;
    mScalesInputIdx = mWeightInputIdx + 1;
    mZerosInputIdx = (quant_algo & ZER0) ? mScalesInputIdx + 1 : mScalesInputIdx;
    mBiasesInputIdx = (quant_algo & BIAS) ? mZerosInputIdx + 1 : mZerosInputIdx;

    if (mType == nvinfer1::DataType::kHALF)
    {
        if (quant_algo & ZER0)
        {
            // has zeros
            m_weightOnlyGroupwiseGemmRunner
                = std::make_shared<tensorrt_llm::kernels::cutlass_kernels::CutlassFpAIntBGemmRunner<half,
                    cutlass::uint4b_t, cutlass::WeightOnlyQuantOp::FINEGRAINED_SCALE_AND_ZEROS>>();
        }
        else
        {
            // no zeros
            m_weightOnlyGroupwiseGemmRunner
                = std::make_shared<tensorrt_llm::kernels::cutlass_kernels::CutlassFpAIntBGemmRunner<half,
                    cutlass::uint4b_t, cutlass::WeightOnlyQuantOp::FINEGRAINED_SCALE_ONLY>>();
        }
    }
    else
    {
        PLUGIN_ASSERT(false);
    }
}

// IPluginV2DynamicExt Methods
nvinfer1::IPluginV2DynamicExt* WeightOnlyGroupwiseQuantMatmulPlugin::clone() const noexcept
{
    auto* plugin = new WeightOnlyGroupwiseQuantMatmulPlugin(mType, mQuantAlgo, mGroupSize);
    plugin->setPluginNamespace(mNamespace.c_str());
    return plugin;
}

nvinfer1::DimsExprs WeightOnlyGroupwiseQuantMatmulPlugin::getOutputDimensions(
    int outputIndex, const nvinfer1::DimsExprs* inputs, int nbInputs, nvinfer1::IExprBuilder& exprBuilder) noexcept
{

    // inputs
    //   0 activations      [M, K]
    //   1 pre-quant scales [K] (optional)
    //   2 weights          [K, N/8]
    //   3 scales           [K // group_size, N]
    //   4 zeros            [K // group_size, N] (optional)
    //   5 biases           [M] (optional)

    try
    {
        PLUGIN_ASSERT(nbInputs == mBiasesInputIdx + 1);
        PLUGIN_ASSERT(outputIndex == 0);
        const int nbDimsA = inputs[0].nbDims;
        const int nbDimsB = inputs[mWeightInputIdx].nbDims;
        PLUGIN_ASSERT(nbDimsA >= 2);
        PLUGIN_ASSERT(nbDimsB == 2);
        DimsExprs ret;
        ret.nbDims = nbDimsA;
        for (int ii = 0; ii < nbDimsA - 1; ++ii)
        {
            ret.d[ii] = inputs[0].d[ii];
        }

        // int4 weight only quant
        ret.d[nbDimsA - 1] = exprBuilder.constant(inputs[mWeightInputIdx].d[1]->getConstantValue() * 8);

        return ret;
    }
    catch (const std::exception& e)
    {
        caughtError(e);
    }
    return DimsExprs{};
}

bool WeightOnlyGroupwiseQuantMatmulPlugin::supportsFormatCombination(
    int pos, const nvinfer1::PluginTensorDesc* inOut, int nbInputs, int nbOutputs) noexcept
{
    if (pos < mBiasesInputIdx + 2)
    {
        if (pos == mWeightInputIdx)
        {
            // weights
            return inOut[mWeightInputIdx].type == nvinfer1::DataType::kFLOAT
                && inOut[mWeightInputIdx].format == TensorFormat::kLINEAR;
        }
        else
        {
            return inOut[pos].type == mType && inOut[pos].format == TensorFormat::kLINEAR;
        }
    }
    else
    {
        // Never should be here
        assert(false);
        return false;
    }
}

void WeightOnlyGroupwiseQuantMatmulPlugin::configurePlugin(const nvinfer1::DynamicPluginTensorDesc* in, int nbInputs,
    const nvinfer1::DynamicPluginTensorDesc* out, int nbOutputs) noexcept
{
    int maxM = 1;
    for (int ii = 0; ii < in[0].max.nbDims - 1; ++ii)
    {
        maxM *= in[0].max.d[ii];
    }
    const int maxK = in[0].max.d[in[0].max.nbDims - 1];
    // int32 packed int4 elements
    const int maxN = in[mWeightInputIdx].max.d[1] * 8;
    int smoothedActSize = maxM * maxK * (in[0].desc.type == nvinfer1::DataType::kFLOAT ? 4 : 2);
    m_workspaceMaxSize = smoothedActSize + m_weightOnlyGroupwiseGemmRunner->getWorkspaceSize(maxM, maxN, maxK);
}

size_t WeightOnlyGroupwiseQuantMatmulPlugin::getWorkspaceSize(const nvinfer1::PluginTensorDesc* inputs, int nbInputs,
    const nvinfer1::PluginTensorDesc* outputs, int nbOutputs) const noexcept
{
    return m_workspaceMaxSize;
}

int WeightOnlyGroupwiseQuantMatmulPlugin::enqueue(const nvinfer1::PluginTensorDesc* inputDesc,
    const nvinfer1::PluginTensorDesc* outputDesc, const void* const* inputs, void* const* outputs, void* workspace,
    cudaStream_t stream) noexcept
{
    // inputs
    //   0 activations      [M, K]
    //   1 pre-quant scales [K]
    //   2 weights          [K, N/8]
    //   3 scales           [K // group_size, N]
    //   4 zeros            [K // group_size, N]
    //   5 biases           [M]
    // outputs
    //   mat                [M, N]

    int m = 1;
    for (int ii = 0; ii < inputDesc[0].dims.nbDims - 1; ++ii)
    {
        m *= inputDesc[0].dims.d[ii];
    }
    const int n = inputDesc[mWeightInputIdx].dims.d[1];
    const int k = inputDesc[0].dims.d[inputDesc[0].dims.nbDims - 1];

    // mQuantAlgo = pre_quant_scale * 4 + zero * 2 + bias
    if (mQuantAlgo & PRE_SCALE_QUANT)
    {
        // Apply pre-quant per channel scale on activations
        tensorrt_llm::kernels::apply_per_channel_scale_kernel_launcher<half>(reinterpret_cast<half*>(workspace),
            reinterpret_cast<const half*>(inputs[0]), reinterpret_cast<const half*>(inputs[mPreQuantScaleInputIdx]), m,
            k);
    }

    const half* zeros_ptr = (mQuantAlgo & ZER0) ? reinterpret_cast<const half*>(inputs[mZerosInputIdx]) : nullptr;
    const half* biases_ptr = (mQuantAlgo & BIAS) ? reinterpret_cast<const half*>(inputs[mBiasesInputIdx]) : nullptr;
    const half* act_ptr = reinterpret_cast<const half*>((mQuantAlgo & PRE_SCALE_QUANT) ? workspace : inputs[0]);

    if (mType == nvinfer1::DataType::kHALF)
    {
        if (m < SMALL_M_FAST_PATH)
        {
            // Use CUDA kernels for small batch size
            tensorrt_llm::kernels::groupwise_weight_only_matmul_i2f_launcher(
                reinterpret_cast<const int32_t*>(inputs[mWeightInputIdx]),
                reinterpret_cast<const half*>(inputs[mScalesInputIdx]), zeros_ptr, act_ptr, biases_ptr,
                reinterpret_cast<half*>(outputs[0]), m, n * 8, k, mGroupSize, &stream);
        }
        else
        {
            // Use cutlass kernels for large batch size
            const int ws_bytes = m_weightOnlyGroupwiseGemmRunner->getWorkspaceSize(m, n, k);

            int32_t* weight_ptr = const_cast<int32_t*>(reinterpret_cast<const int32_t*>(inputs[mWeightInputIdx]));

            m_weightOnlyGroupwiseGemmRunner->gemm(act_ptr, reinterpret_cast<cutlass::uint4b_t*>(weight_ptr),
                reinterpret_cast<const half*>(inputs[mScalesInputIdx]), zeros_ptr, biases_ptr,
                reinterpret_cast<half*>(outputs[0]), m, n * 8, k, mGroupSize,
                reinterpret_cast<char*>(workspace + m * k * sizeof(half)), ws_bytes, stream);
        }
    }
    else
    {
        assert(false);
    }

    return 0;
}

// IPluginV2Ext Methods
nvinfer1::DataType WeightOnlyGroupwiseQuantMatmulPlugin::getOutputDataType(
    int index, const nvinfer1::DataType* inputTypes, int nbInputs) const noexcept
{
    PLUGIN_ASSERT(index == 0);
    return mType;
}

// IPluginV2 Methods

const char* WeightOnlyGroupwiseQuantMatmulPlugin::getPluginType() const noexcept
{
    return WOQ_GROUPWISE_MATMUL_PLUGIN_NAME;
}

const char* WeightOnlyGroupwiseQuantMatmulPlugin::getPluginVersion() const noexcept
{
    return WOQ_GROUPWISE_MATMUL_PLUGIN_VERSION;
}

int WeightOnlyGroupwiseQuantMatmulPlugin::getNbOutputs() const noexcept
{
    return 1;
}

int WeightOnlyGroupwiseQuantMatmulPlugin::initialize() noexcept
{
    return 0;
}

void WeightOnlyGroupwiseQuantMatmulPlugin::terminate() noexcept {}

size_t WeightOnlyGroupwiseQuantMatmulPlugin::getSerializationSize() const noexcept
{
    return 2 * sizeof(int) + sizeof(nvinfer1::DataType);
}

void WeightOnlyGroupwiseQuantMatmulPlugin::serialize(void* buffer) const noexcept
{
    char *d = static_cast<char*>(buffer), *a = d;
    write(d, mType);
    write(d, mQuantAlgo);
    write(d, mGroupSize);
    assert(d == a + getSerializationSize());
}

void WeightOnlyGroupwiseQuantMatmulPlugin::destroy() noexcept
{
    // This gets called when the network containing plugin is destroyed
    delete this;
}

void WeightOnlyGroupwiseQuantMatmulPlugin::setPluginNamespace(const char* libNamespace) noexcept
{
    mNamespace = libNamespace;
}

const char* WeightOnlyGroupwiseQuantMatmulPlugin::getPluginNamespace() const noexcept
{
    return mNamespace.c_str();
}

///////////////

WeightOnlyGroupwiseQuantMatmulPluginCreator::WeightOnlyGroupwiseQuantMatmulPluginCreator()
{
    // Fill PluginFieldCollection with PluginField arguments metadata
    mPluginAttributes.clear();
    mPluginAttributes.emplace_back(PluginField("type_id", nullptr, PluginFieldType::kINT32, 1));
    mPluginAttributes.emplace_back(PluginField("quant_algo", nullptr, PluginFieldType::kINT32, 1));
    mPluginAttributes.emplace_back(PluginField("group_size", nullptr, PluginFieldType::kINT32, 1));
    mFC.nbFields = mPluginAttributes.size();
    mFC.fields = mPluginAttributes.data();
}

const char* WeightOnlyGroupwiseQuantMatmulPluginCreator::getPluginName() const noexcept
{
    return WOQ_GROUPWISE_MATMUL_PLUGIN_NAME;
}

const char* WeightOnlyGroupwiseQuantMatmulPluginCreator::getPluginVersion() const noexcept
{
    return WOQ_GROUPWISE_MATMUL_PLUGIN_VERSION;
}

const PluginFieldCollection* WeightOnlyGroupwiseQuantMatmulPluginCreator::getFieldNames() noexcept
{
    return &mFC;
}

IPluginV2* WeightOnlyGroupwiseQuantMatmulPluginCreator::createPlugin(
    const char* name, const PluginFieldCollection* fc) noexcept
{
    const PluginField* fields = fc->fields;
    nvinfer1::DataType type;
    int QuantAlgo;
    int GroupSize;
    // Read configurations from each fields
    for (int i = 0; i < fc->nbFields; ++i)
    {
        const char* attrName = fields[i].name;
        if (!strcmp(attrName, "quant_algo"))
        {
            PLUGIN_ASSERT(fields[i].type == PluginFieldType::kINT32);
            QuantAlgo = static_cast<int>(*(static_cast<const int*>(fields[i].data)));
        }
        else if (!strcmp(attrName, "group_size"))
        {
            PLUGIN_ASSERT(fields[i].type == PluginFieldType::kINT32);
            GroupSize = static_cast<int>(*(static_cast<const int*>(fields[i].data)));
        }
        else if (!strcmp(attrName, "type_id"))
        {
            PLUGIN_ASSERT(fields[i].type == PluginFieldType::kINT32);
            type = static_cast<nvinfer1::DataType>(*(static_cast<const nvinfer1::DataType*>(fields[i].data)));
        }
    }
    try
    {
        auto* obj = new WeightOnlyGroupwiseQuantMatmulPlugin(type, QuantAlgo, GroupSize);
        obj->setPluginNamespace(mNamespace.c_str());
        return obj;
    }
    catch (const std::exception& e)
    {
        caughtError(e);
    }
    return nullptr;
}

IPluginV2* WeightOnlyGroupwiseQuantMatmulPluginCreator::deserializePlugin(
    const char* name, const void* serialData, size_t serialLength) noexcept
{
    // This object will be deleted when the network is destroyed, which will
    // call weightOnlyGroupwiseQuantMatmulPlugin::destroy()
    try
    {
        auto* obj = new WeightOnlyGroupwiseQuantMatmulPlugin(serialData, serialLength);
        obj->setPluginNamespace(mNamespace.c_str());
        return obj;
    }
    catch (const std::exception& e)
    {
        caughtError(e);
    }
    return nullptr;
}

void WeightOnlyGroupwiseQuantMatmulPluginCreator::setPluginNamespace(const char* libNamespace) noexcept
{
    mNamespace = libNamespace;
}

const char* WeightOnlyGroupwiseQuantMatmulPluginCreator::getPluginNamespace() const noexcept
{
    return mNamespace.c_str();
}