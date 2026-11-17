#pragma once
#include <pch.h>
#include <vulkan/vulkan.h>
#include <string>
#include <vector>

#include <shaders/Shader_Vk.h>

class DepthTransfer_Vk : public Shader_Vk
{
    bool _isInteger = false;

    uint32_t InNumThreadsX = 16;
    uint32_t InNumThreadsY = 16;

  public:
    DepthTransfer_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice,
                     VkFormat InFormat = VK_FORMAT_UNDEFINED);
    ~DepthTransfer_Vk() = default;

    bool Dispatch(VkDevice InDevice, VkCommandBuffer InCmdList, VkImageView InResourceView, VkImageView OutResourceView,
                  VkExtent2D OutExtent);

};
