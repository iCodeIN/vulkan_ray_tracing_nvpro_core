/*
 * Copyright (c) 2021, NVIDIA CORPORATION.  All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-FileCopyrightText: Copyright (c) 2014-2021 NVIDIA CORPORATION
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sbtwrapper_vk.hpp"

using namespace nvvk;

//--------------------------------------------------------------------------------------------------
// Default setup
//
void SBTWrapper::setup(VkDevice device, uint32_t familyIndex, nvvk::ResourceAllocator* allocator, const VkPhysicalDeviceRayTracingPipelinePropertiesKHR& rt_properties)
{
  m_device     = device;
  m_queueIndex = familyIndex;
  m_pAlloc     = allocator;
  m_debug.setup(device);

  m_handleSize     = rt_properties.shaderGroupHandleSize;  // Size of a program identifier
  m_baseAlignment = rt_properties.shaderGroupBaseAlignment;
}

//--------------------------------------------------------------------------------------------------
// Destroying the allocated buffers and clearing all vectors
//
void SBTWrapper::destroy()
{
  for(auto& b : m_buffer)
    m_pAlloc->destroy(b);

  for(auto& i : m_index)
    i = {};
}

//--------------------------------------------------------------------------------------------------
// Finding the handle index position of each group type in the pipeline creation info.
// If the pipeline was created like: raygen, miss, hit, miss, hit, hit
// The result will be: raygen[0], miss[1, 3], hit[2, 4, 5], callable[]
//
void SBTWrapper::addIndices(VkRayTracingPipelineCreateInfoKHR rayPipelineInfo)
{
  for(auto& i : m_index)
    i = {};

  uint32_t stageIdx = 0;

  // Finding the handle position of each group, splitting by raygen, miss and hit group
  for(uint32_t g = 0; g < rayPipelineInfo.groupCount; g++)
  {
    if(rayPipelineInfo.pGroups[g].type == VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR)
    {
      if(rayPipelineInfo.pStages[stageIdx].stage == VK_SHADER_STAGE_RAYGEN_BIT_KHR)
      {
        m_index[eRaygen].push_back(g);
        stageIdx++;
      }
      else if(rayPipelineInfo.pStages[stageIdx].stage == VK_SHADER_STAGE_MISS_BIT_KHR)
      {
        m_index[eMiss].push_back(g);
        stageIdx++;
      }
      else if(rayPipelineInfo.pStages[stageIdx].stage == VK_SHADER_STAGE_CALLABLE_BIT_KHR)
      {
        m_index[eCallable].push_back(g);
        stageIdx++;
      }
    }
    else
    {
      m_index[eHit].push_back(g);
      if(rayPipelineInfo.pGroups[g].closestHitShader != VK_SHADER_UNUSED_KHR)
        stageIdx++;
      if(rayPipelineInfo.pGroups[g].anyHitShader != VK_SHADER_UNUSED_KHR)
        stageIdx++;
      if(rayPipelineInfo.pGroups[g].intersectionShader != VK_SHADER_UNUSED_KHR)
        stageIdx++;
    }
  }
}

//--------------------------------------------------------------------------------------------------
// This function creates 4 buffers, for raygen, miss, hit and callable shader.
// Each buffer will have the handle + 'data (if any)', .. n-times they have entries in the pipeline.
//
void SBTWrapper::create(VkPipeline rtPipeline, VkRayTracingPipelineCreateInfoKHR rayPipelineInfo /*= {}*/)
{
  for(auto& b : m_buffer)
    m_pAlloc->destroy(b);

  // Get the total number of groups and handle index position
  uint32_t groupCount{0};
  if(rayPipelineInfo.sType == VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR)
  {
    addIndices(rayPipelineInfo);
    groupCount = rayPipelineInfo.groupCount;
  }
  else
  {
    // Find how many groups when added manually, by finding the largest index and adding 1
    // See also addIndex for manual entries
    for(auto& i : m_index)
    {
      if(!i.empty())
        groupCount = std::max(groupCount, *std::max_element(std::begin(i), std::end(i)));
    }
    groupCount++;
  }

  // Fetch all the shader handles used in the pipeline, so that they can be written in the SBT
  uint32_t             sbtSize = groupCount * m_handleSize;
  std::vector<uint8_t> shaderHandleStorage(sbtSize);
  auto result = vkGetRayTracingShaderGroupHandlesKHR(m_device, rtPipeline, 0, groupCount, sbtSize, shaderHandleStorage.data());

  // Find the max stride, minimum is the handle size + size of 'data (if any)' aligned to shaderGroupBaseAlignment
  auto findStride = [&](auto entry, auto& stride) {
    stride = nvh::align_up(m_handleSize, m_baseAlignment);  // minimum stride
    for(auto& e : entry)
    {
      // Find the largest data + handle size, all aligned
      uint32_t dataHandleSize =
          nvh::align_up(static_cast<uint32_t>(m_handleSize + e.second.size() * sizeof(uint8_t)), m_baseAlignment);
      stride = std::max(stride, dataHandleSize);
    }
  };
  findStride(m_data[eRaygen], m_stride[eRaygen]);
  findStride(m_data[eMiss], m_stride[eMiss]);
  findStride(m_data[eHit], m_stride[eHit]);
  findStride(m_data[eCallable], m_stride[eCallable]);

  // Buffer holding the staging information
  std::array<std::vector<uint8_t>, 4> stage;
  stage[eRaygen]   = std::vector<uint8_t>(m_stride[eRaygen] * indexCount(eRaygen));
  stage[eMiss]     = std::vector<uint8_t>(m_stride[eMiss] * indexCount(eMiss));
  stage[eHit]      = std::vector<uint8_t>(m_stride[eHit] * indexCount(eHit));
  stage[eCallable] = std::vector<uint8_t>(m_stride[eCallable] * indexCount(eCallable));

  // Write the handles in the SBT buffer + data info (if any)
  auto copyHandles = [&](std::vector<uint8_t>& buffer, std::vector<uint32_t>& indices, uint32_t stride, auto& data) {
    auto* pBuffer = buffer.data();
    for(uint32_t index = 0; index < static_cast<uint32_t>(indices.size()); index++)
    {
      auto* pStart = pBuffer;
      // Copy the handle
      memcpy(pBuffer, shaderHandleStorage.data() + (indices[index] * m_handleSize), m_handleSize);
      // If there is data for this group index, copy it too
      auto it = data.find(index);
      if(it != std::end(data))
      {
        pBuffer += m_handleSize;
        memcpy(pBuffer, it->second.data(), it->second.size() * sizeof(uint8_t));
      }
      pBuffer = pStart + stride;  // Jumping to next group
    }
  };

  // Copy the handles/data to each staging buffer
  copyHandles(stage[eRaygen], m_index[eRaygen], m_stride[eRaygen], m_data[eRaygen]);
  copyHandles(stage[eMiss], m_index[eMiss], m_stride[eMiss], m_data[eMiss]);
  copyHandles(stage[eHit], m_index[eHit], m_stride[eHit], m_data[eHit]);
  copyHandles(stage[eCallable], m_index[eCallable], m_stride[eCallable], m_data[eCallable]);

  // Creating device local buffers where handles will be stored
  auto              usage_flags = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  auto              mem_flags   = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  nvvk::CommandPool genCmdBuf(m_device, m_queueIndex);
  VkCommandBuffer   cmdBuf = genCmdBuf.createCommandBuffer();
  for(uint32_t i = 0; i < 4; i++)
  {
    if(!stage[i].empty())
    {
      m_buffer[i] = m_pAlloc->createBuffer(cmdBuf, stage[i], usage_flags, mem_flags);
      NAME_IDX_VK(m_buffer[i].buffer, i);
    }
  }
  genCmdBuf.submitAndWait(cmdBuf);
  m_pAlloc->finalizeAndReleaseStaging();
}

VkDeviceAddress SBTWrapper::getAddress(GroupType t)
{
  if (m_buffer[t].buffer == VK_NULL_HANDLE)
    return 0;
  VkBufferDeviceAddressInfo i{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, nullptr, m_buffer[t].buffer};
  return vkGetBufferDeviceAddress(m_device, &i);
}

VkStridedDeviceAddressRegionKHR SBTWrapper::getRegion(GroupType t)
{
  return VkStridedDeviceAddressRegionKHR{getAddress(t), getStride(t), getSize(t)};
}

std::array<VkStridedDeviceAddressRegionKHR, 4> SBTWrapper::getRegions()
{
  std::array<VkStridedDeviceAddressRegionKHR, 4> regions{getRegion(eRaygen), getRegion(eMiss), getRegion(eHit), getRegion(eCallable)};
  return regions;
}