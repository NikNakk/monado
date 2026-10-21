// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: MIT
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define CHECK(x) do { VkResult r_=(x); if(r_ != VK_SUCCESS) { fprintf(stderr,"%s:%d %s = %d\n",__FILE__,__LINE__,#x,r_); exit(2); } } while(0)
#define SIZE 32
static PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
static PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets;
static PFN_vkAllocateMemory vkAllocateMemory;
static PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
static PFN_vkBindBufferMemory vkBindBufferMemory;
static PFN_vkBindImageMemory vkBindImageMemory;
static PFN_vkCmdBeginRenderPass vkCmdBeginRenderPass;
static PFN_vkCmdBeginRendering vkCmdBeginRendering;
static PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets;
static PFN_vkCmdBindPipeline vkCmdBindPipeline;
static PFN_vkCmdCopyImageToBuffer vkCmdCopyImageToBuffer;
static PFN_vkCmdDraw vkCmdDraw;
static PFN_vkCmdEndRenderPass vkCmdEndRenderPass;
static PFN_vkCmdEndRendering vkCmdEndRendering;
static PFN_vkCmdNextSubpass vkCmdNextSubpass;
static PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
static PFN_vkCreateBuffer vkCreateBuffer;
static PFN_vkCreateCommandPool vkCreateCommandPool;
static PFN_vkCreateDescriptorPool vkCreateDescriptorPool;
static PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout;
static PFN_vkCreateDevice vkCreateDevice;
static PFN_vkCreateFramebuffer vkCreateFramebuffer;
static PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines;
static PFN_vkCreateImage vkCreateImage;
static PFN_vkCreateImageView vkCreateImageView;
static PFN_vkCreateInstance vkCreateInstance;
static PFN_vkCreatePipelineLayout vkCreatePipelineLayout;
static PFN_vkCreateRenderPass vkCreateRenderPass;
static PFN_vkCreateShaderModule vkCreateShaderModule;
static PFN_vkDestroyBuffer vkDestroyBuffer;
static PFN_vkDestroyCommandPool vkDestroyCommandPool;
static PFN_vkDestroyDescriptorPool vkDestroyDescriptorPool;
static PFN_vkDestroyDescriptorSetLayout vkDestroyDescriptorSetLayout;
static PFN_vkDestroyDevice vkDestroyDevice;
static PFN_vkDestroyFramebuffer vkDestroyFramebuffer;
static PFN_vkDestroyImage vkDestroyImage;
static PFN_vkDestroyImageView vkDestroyImageView;
static PFN_vkDestroyInstance vkDestroyInstance;
static PFN_vkDestroyPipeline vkDestroyPipeline;
static PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout;
static PFN_vkDestroyRenderPass vkDestroyRenderPass;
static PFN_vkDestroyShaderModule vkDestroyShaderModule;
static PFN_vkEndCommandBuffer vkEndCommandBuffer;
static PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties;
static PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices;
static PFN_vkFreeCommandBuffers vkFreeCommandBuffers;
static PFN_vkFreeMemory vkFreeMemory;
static PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
static PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;
static PFN_vkGetDeviceQueue vkGetDeviceQueue;
static PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
static PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
static PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties;
static PFN_vkMapMemory vkMapMemory;
static PFN_vkQueueSubmit vkQueueSubmit;
static PFN_vkQueueWaitIdle vkQueueWaitIdle;
static PFN_vkUnmapMemory vkUnmapMemory;
static PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets;
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
static void load_dispatch(VkInstance instance) {
    vkAllocateCommandBuffers=(PFN_vkAllocateCommandBuffers)vkGetInstanceProcAddr(instance,"vkAllocateCommandBuffers"); if(!vkAllocateCommandBuffers) {fprintf(stderr,"Missing vkAllocateCommandBuffers\n");exit(2);}
    vkAllocateDescriptorSets=(PFN_vkAllocateDescriptorSets)vkGetInstanceProcAddr(instance,"vkAllocateDescriptorSets"); if(!vkAllocateDescriptorSets) {fprintf(stderr,"Missing vkAllocateDescriptorSets\n");exit(2);}
    vkAllocateMemory=(PFN_vkAllocateMemory)vkGetInstanceProcAddr(instance,"vkAllocateMemory"); if(!vkAllocateMemory) {fprintf(stderr,"Missing vkAllocateMemory\n");exit(2);}
    vkBeginCommandBuffer=(PFN_vkBeginCommandBuffer)vkGetInstanceProcAddr(instance,"vkBeginCommandBuffer"); if(!vkBeginCommandBuffer) {fprintf(stderr,"Missing vkBeginCommandBuffer\n");exit(2);}
    vkBindBufferMemory=(PFN_vkBindBufferMemory)vkGetInstanceProcAddr(instance,"vkBindBufferMemory"); if(!vkBindBufferMemory) {fprintf(stderr,"Missing vkBindBufferMemory\n");exit(2);}
    vkBindImageMemory=(PFN_vkBindImageMemory)vkGetInstanceProcAddr(instance,"vkBindImageMemory"); if(!vkBindImageMemory) {fprintf(stderr,"Missing vkBindImageMemory\n");exit(2);}
    vkCmdBeginRenderPass=(PFN_vkCmdBeginRenderPass)vkGetInstanceProcAddr(instance,"vkCmdBeginRenderPass"); if(!vkCmdBeginRenderPass) {fprintf(stderr,"Missing vkCmdBeginRenderPass\n");exit(2);}
    vkCmdBeginRendering=(PFN_vkCmdBeginRendering)vkGetInstanceProcAddr(instance,"vkCmdBeginRendering"); if(!vkCmdBeginRendering) {fprintf(stderr,"Missing vkCmdBeginRendering\n");exit(2);}
    vkCmdBindDescriptorSets=(PFN_vkCmdBindDescriptorSets)vkGetInstanceProcAddr(instance,"vkCmdBindDescriptorSets"); if(!vkCmdBindDescriptorSets) {fprintf(stderr,"Missing vkCmdBindDescriptorSets\n");exit(2);}
    vkCmdBindPipeline=(PFN_vkCmdBindPipeline)vkGetInstanceProcAddr(instance,"vkCmdBindPipeline"); if(!vkCmdBindPipeline) {fprintf(stderr,"Missing vkCmdBindPipeline\n");exit(2);}
    vkCmdCopyImageToBuffer=(PFN_vkCmdCopyImageToBuffer)vkGetInstanceProcAddr(instance,"vkCmdCopyImageToBuffer"); if(!vkCmdCopyImageToBuffer) {fprintf(stderr,"Missing vkCmdCopyImageToBuffer\n");exit(2);}
    vkCmdDraw=(PFN_vkCmdDraw)vkGetInstanceProcAddr(instance,"vkCmdDraw"); if(!vkCmdDraw) {fprintf(stderr,"Missing vkCmdDraw\n");exit(2);}
    vkCmdEndRenderPass=(PFN_vkCmdEndRenderPass)vkGetInstanceProcAddr(instance,"vkCmdEndRenderPass"); if(!vkCmdEndRenderPass) {fprintf(stderr,"Missing vkCmdEndRenderPass\n");exit(2);}
    vkCmdEndRendering=(PFN_vkCmdEndRendering)vkGetInstanceProcAddr(instance,"vkCmdEndRendering"); if(!vkCmdEndRendering) {fprintf(stderr,"Missing vkCmdEndRendering\n");exit(2);}
    vkCmdNextSubpass=(PFN_vkCmdNextSubpass)vkGetInstanceProcAddr(instance,"vkCmdNextSubpass"); if(!vkCmdNextSubpass) {fprintf(stderr,"Missing vkCmdNextSubpass\n");exit(2);}
    vkCmdPipelineBarrier=(PFN_vkCmdPipelineBarrier)vkGetInstanceProcAddr(instance,"vkCmdPipelineBarrier"); if(!vkCmdPipelineBarrier) {fprintf(stderr,"Missing vkCmdPipelineBarrier\n");exit(2);}
    vkCreateBuffer=(PFN_vkCreateBuffer)vkGetInstanceProcAddr(instance,"vkCreateBuffer"); if(!vkCreateBuffer) {fprintf(stderr,"Missing vkCreateBuffer\n");exit(2);}
    vkCreateCommandPool=(PFN_vkCreateCommandPool)vkGetInstanceProcAddr(instance,"vkCreateCommandPool"); if(!vkCreateCommandPool) {fprintf(stderr,"Missing vkCreateCommandPool\n");exit(2);}
    vkCreateDescriptorPool=(PFN_vkCreateDescriptorPool)vkGetInstanceProcAddr(instance,"vkCreateDescriptorPool"); if(!vkCreateDescriptorPool) {fprintf(stderr,"Missing vkCreateDescriptorPool\n");exit(2);}
    vkCreateDescriptorSetLayout=(PFN_vkCreateDescriptorSetLayout)vkGetInstanceProcAddr(instance,"vkCreateDescriptorSetLayout"); if(!vkCreateDescriptorSetLayout) {fprintf(stderr,"Missing vkCreateDescriptorSetLayout\n");exit(2);}
    vkCreateDevice=(PFN_vkCreateDevice)vkGetInstanceProcAddr(instance,"vkCreateDevice"); if(!vkCreateDevice) {fprintf(stderr,"Missing vkCreateDevice\n");exit(2);}
    vkCreateFramebuffer=(PFN_vkCreateFramebuffer)vkGetInstanceProcAddr(instance,"vkCreateFramebuffer"); if(!vkCreateFramebuffer) {fprintf(stderr,"Missing vkCreateFramebuffer\n");exit(2);}
    vkCreateGraphicsPipelines=(PFN_vkCreateGraphicsPipelines)vkGetInstanceProcAddr(instance,"vkCreateGraphicsPipelines"); if(!vkCreateGraphicsPipelines) {fprintf(stderr,"Missing vkCreateGraphicsPipelines\n");exit(2);}
    vkCreateImage=(PFN_vkCreateImage)vkGetInstanceProcAddr(instance,"vkCreateImage"); if(!vkCreateImage) {fprintf(stderr,"Missing vkCreateImage\n");exit(2);}
    vkCreateImageView=(PFN_vkCreateImageView)vkGetInstanceProcAddr(instance,"vkCreateImageView"); if(!vkCreateImageView) {fprintf(stderr,"Missing vkCreateImageView\n");exit(2);}
    vkCreatePipelineLayout=(PFN_vkCreatePipelineLayout)vkGetInstanceProcAddr(instance,"vkCreatePipelineLayout"); if(!vkCreatePipelineLayout) {fprintf(stderr,"Missing vkCreatePipelineLayout\n");exit(2);}
    vkCreateRenderPass=(PFN_vkCreateRenderPass)vkGetInstanceProcAddr(instance,"vkCreateRenderPass"); if(!vkCreateRenderPass) {fprintf(stderr,"Missing vkCreateRenderPass\n");exit(2);}
    vkCreateShaderModule=(PFN_vkCreateShaderModule)vkGetInstanceProcAddr(instance,"vkCreateShaderModule"); if(!vkCreateShaderModule) {fprintf(stderr,"Missing vkCreateShaderModule\n");exit(2);}
    vkDestroyBuffer=(PFN_vkDestroyBuffer)vkGetInstanceProcAddr(instance,"vkDestroyBuffer"); if(!vkDestroyBuffer) {fprintf(stderr,"Missing vkDestroyBuffer\n");exit(2);}
    vkDestroyCommandPool=(PFN_vkDestroyCommandPool)vkGetInstanceProcAddr(instance,"vkDestroyCommandPool"); if(!vkDestroyCommandPool) {fprintf(stderr,"Missing vkDestroyCommandPool\n");exit(2);}
    vkDestroyDescriptorPool=(PFN_vkDestroyDescriptorPool)vkGetInstanceProcAddr(instance,"vkDestroyDescriptorPool"); if(!vkDestroyDescriptorPool) {fprintf(stderr,"Missing vkDestroyDescriptorPool\n");exit(2);}
    vkDestroyDescriptorSetLayout=(PFN_vkDestroyDescriptorSetLayout)vkGetInstanceProcAddr(instance,"vkDestroyDescriptorSetLayout"); if(!vkDestroyDescriptorSetLayout) {fprintf(stderr,"Missing vkDestroyDescriptorSetLayout\n");exit(2);}
    vkDestroyDevice=(PFN_vkDestroyDevice)vkGetInstanceProcAddr(instance,"vkDestroyDevice"); if(!vkDestroyDevice) {fprintf(stderr,"Missing vkDestroyDevice\n");exit(2);}
    vkDestroyFramebuffer=(PFN_vkDestroyFramebuffer)vkGetInstanceProcAddr(instance,"vkDestroyFramebuffer"); if(!vkDestroyFramebuffer) {fprintf(stderr,"Missing vkDestroyFramebuffer\n");exit(2);}
    vkDestroyImage=(PFN_vkDestroyImage)vkGetInstanceProcAddr(instance,"vkDestroyImage"); if(!vkDestroyImage) {fprintf(stderr,"Missing vkDestroyImage\n");exit(2);}
    vkDestroyImageView=(PFN_vkDestroyImageView)vkGetInstanceProcAddr(instance,"vkDestroyImageView"); if(!vkDestroyImageView) {fprintf(stderr,"Missing vkDestroyImageView\n");exit(2);}
    vkDestroyInstance=(PFN_vkDestroyInstance)vkGetInstanceProcAddr(instance,"vkDestroyInstance"); if(!vkDestroyInstance) {fprintf(stderr,"Missing vkDestroyInstance\n");exit(2);}
    vkDestroyPipeline=(PFN_vkDestroyPipeline)vkGetInstanceProcAddr(instance,"vkDestroyPipeline"); if(!vkDestroyPipeline) {fprintf(stderr,"Missing vkDestroyPipeline\n");exit(2);}
    vkDestroyPipelineLayout=(PFN_vkDestroyPipelineLayout)vkGetInstanceProcAddr(instance,"vkDestroyPipelineLayout"); if(!vkDestroyPipelineLayout) {fprintf(stderr,"Missing vkDestroyPipelineLayout\n");exit(2);}
    vkDestroyRenderPass=(PFN_vkDestroyRenderPass)vkGetInstanceProcAddr(instance,"vkDestroyRenderPass"); if(!vkDestroyRenderPass) {fprintf(stderr,"Missing vkDestroyRenderPass\n");exit(2);}
    vkDestroyShaderModule=(PFN_vkDestroyShaderModule)vkGetInstanceProcAddr(instance,"vkDestroyShaderModule"); if(!vkDestroyShaderModule) {fprintf(stderr,"Missing vkDestroyShaderModule\n");exit(2);}
    vkEndCommandBuffer=(PFN_vkEndCommandBuffer)vkGetInstanceProcAddr(instance,"vkEndCommandBuffer"); if(!vkEndCommandBuffer) {fprintf(stderr,"Missing vkEndCommandBuffer\n");exit(2);}
    vkEnumerateDeviceExtensionProperties=(PFN_vkEnumerateDeviceExtensionProperties)vkGetInstanceProcAddr(instance,"vkEnumerateDeviceExtensionProperties"); if(!vkEnumerateDeviceExtensionProperties) {fprintf(stderr,"Missing vkEnumerateDeviceExtensionProperties\n");exit(2);}
    vkEnumeratePhysicalDevices=(PFN_vkEnumeratePhysicalDevices)vkGetInstanceProcAddr(instance,"vkEnumeratePhysicalDevices"); if(!vkEnumeratePhysicalDevices) {fprintf(stderr,"Missing vkEnumeratePhysicalDevices\n");exit(2);}
    vkFreeCommandBuffers=(PFN_vkFreeCommandBuffers)vkGetInstanceProcAddr(instance,"vkFreeCommandBuffers"); if(!vkFreeCommandBuffers) {fprintf(stderr,"Missing vkFreeCommandBuffers\n");exit(2);}
    vkFreeMemory=(PFN_vkFreeMemory)vkGetInstanceProcAddr(instance,"vkFreeMemory"); if(!vkFreeMemory) {fprintf(stderr,"Missing vkFreeMemory\n");exit(2);}
    vkGetBufferMemoryRequirements=(PFN_vkGetBufferMemoryRequirements)vkGetInstanceProcAddr(instance,"vkGetBufferMemoryRequirements"); if(!vkGetBufferMemoryRequirements) {fprintf(stderr,"Missing vkGetBufferMemoryRequirements\n");exit(2);}
    vkGetDeviceProcAddr=(PFN_vkGetDeviceProcAddr)vkGetInstanceProcAddr(instance,"vkGetDeviceProcAddr"); if(!vkGetDeviceProcAddr) {fprintf(stderr,"Missing vkGetDeviceProcAddr\n");exit(2);}
    vkGetDeviceQueue=(PFN_vkGetDeviceQueue)vkGetInstanceProcAddr(instance,"vkGetDeviceQueue"); if(!vkGetDeviceQueue) {fprintf(stderr,"Missing vkGetDeviceQueue\n");exit(2);}
    vkGetImageMemoryRequirements=(PFN_vkGetImageMemoryRequirements)vkGetInstanceProcAddr(instance,"vkGetImageMemoryRequirements"); if(!vkGetImageMemoryRequirements) {fprintf(stderr,"Missing vkGetImageMemoryRequirements\n");exit(2);}
    vkGetPhysicalDeviceMemoryProperties=(PFN_vkGetPhysicalDeviceMemoryProperties)vkGetInstanceProcAddr(instance,"vkGetPhysicalDeviceMemoryProperties"); if(!vkGetPhysicalDeviceMemoryProperties) {fprintf(stderr,"Missing vkGetPhysicalDeviceMemoryProperties\n");exit(2);}
    vkGetPhysicalDeviceProperties2=(PFN_vkGetPhysicalDeviceProperties2)vkGetInstanceProcAddr(instance,"vkGetPhysicalDeviceProperties2"); if(!vkGetPhysicalDeviceProperties2) {fprintf(stderr,"Missing vkGetPhysicalDeviceProperties2\n");exit(2);}
    vkGetPhysicalDeviceQueueFamilyProperties=(PFN_vkGetPhysicalDeviceQueueFamilyProperties)vkGetInstanceProcAddr(instance,"vkGetPhysicalDeviceQueueFamilyProperties"); if(!vkGetPhysicalDeviceQueueFamilyProperties) {fprintf(stderr,"Missing vkGetPhysicalDeviceQueueFamilyProperties\n");exit(2);}
    vkMapMemory=(PFN_vkMapMemory)vkGetInstanceProcAddr(instance,"vkMapMemory"); if(!vkMapMemory) {fprintf(stderr,"Missing vkMapMemory\n");exit(2);}
    vkQueueSubmit=(PFN_vkQueueSubmit)vkGetInstanceProcAddr(instance,"vkQueueSubmit"); if(!vkQueueSubmit) {fprintf(stderr,"Missing vkQueueSubmit\n");exit(2);}
    vkQueueWaitIdle=(PFN_vkQueueWaitIdle)vkGetInstanceProcAddr(instance,"vkQueueWaitIdle"); if(!vkQueueWaitIdle) {fprintf(stderr,"Missing vkQueueWaitIdle\n");exit(2);}
    vkUnmapMemory=(PFN_vkUnmapMemory)vkGetInstanceProcAddr(instance,"vkUnmapMemory"); if(!vkUnmapMemory) {fprintf(stderr,"Missing vkUnmapMemory\n");exit(2);}
    vkUpdateDescriptorSets=(PFN_vkUpdateDescriptorSets)vkGetInstanceProcAddr(instance,"vkUpdateDescriptorSets"); if(!vkUpdateDescriptorSets) {fprintf(stderr,"Missing vkUpdateDescriptorSets\n");exit(2);}
}
const char *kl_driver_target_name(void) { return "owned-resolve-probe"; }
void kl_glfb_note_eye_mtl_texture(int a,int b,void*c,int d,int e,int f) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;abort(); }

static VkDevice device;
static VkPhysicalDeviceMemoryProperties memory;
static VkShaderModule vert, fill, resolve[3];
static VkPipelineLayout layout;
static VkDescriptorSetLayout set_layout;
static VkQueue queue;
static VkCommandPool pool;
static int custom_available;
static int with_depth;
static int stereo;
static int suspended;
static PFN_vkCmdClearDepthStencilImage vkCmdClearDepthStencilImage;
typedef struct { VkImage image; VkImageView view; VkDeviceMemory memory; } Image;

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i=0;i<memory.memoryTypeCount;i++)
        if((bits&(1u<<i)) && (memory.memoryTypes[i].propertyFlags&flags)==flags) return i;
    fprintf(stderr,"No memory type for %#x/%#x\n",bits,flags); exit(2);
}
static Image image_format(VkSampleCountFlagBits samples, VkImageUsageFlags usage,VkFormat format,VkImageAspectFlags aspect) {
    Image out={0};
    VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
        .format=format,.extent={SIZE,SIZE,1},.mipLevels=1,.arrayLayers=stereo?2:1,
        .samples=samples,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=usage,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    CHECK(vkCreateImage(device,&ci,NULL,&out.image));
    VkMemoryRequirements req; vkGetImageMemoryRequirements(device,out.image,&req);
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,
        .memoryTypeIndex=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    CHECK(vkAllocateMemory(device,&ai,NULL,&out.memory)); CHECK(vkBindImageMemory(device,out.image,out.memory,0));
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=out.image,
        .viewType=stereo?VK_IMAGE_VIEW_TYPE_2D_ARRAY:VK_IMAGE_VIEW_TYPE_2D,.format=ci.format,.subresourceRange={aspect,0,1,0,stereo?2:1}};
    CHECK(vkCreateImageView(device,&vi,NULL,&out.view)); return out;
}
static Image image(VkSampleCountFlagBits samples,VkImageUsageFlags usage) { return image_format(samples,usage,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_ASPECT_COLOR_BIT); }
static void destroy_image(Image a) {
    vkDestroyImageView(device,a.view,NULL); vkDestroyImage(device,a.image,NULL); vkFreeMemory(device,a.memory,NULL);
}
static VkShaderModule shader(const char *root,const char *name) {
    char path[4096]; snprintf(path,sizeof path,"%s/%s.spv",root,name);
    FILE *f=fopen(path,"rb"); if(!f) {perror(path);exit(2);}
    fseek(f,0,SEEK_END); long n=ftell(f); rewind(f); uint32_t *bytes=malloc(n);
    if(fread(bytes,1,n,f)!=(size_t)n) exit(2); fclose(f);
    VkShaderModuleCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=n,.pCode=bytes};
    VkShaderModule out; CHECK(vkCreateShaderModule(device,&ci,NULL,&out)); free(bytes); return out;
}
static VkPipeline pipeline(VkRenderPass pass,uint32_t subpass,int dynamic,int is_resolve,int samples) {
    VkPipelineShaderStageCreateInfo stages[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vert,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,
         .module=is_resolve?resolve[samples==4?1:samples==2?2:0]:fill,.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport viewport={0,0,SIZE,SIZE,0,1}; VkRect2D scissor={{0,0},{SIZE,SIZE}};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.pViewports=&viewport,.scissorCount=1,.pScissors=&scissor};
    VkPipelineRasterizationStateCreateInfo rs={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=is_resolve?VK_SAMPLE_COUNT_1_BIT:samples};
    VkPipelineColorBlendAttachmentState ba={.colorWriteMask=15};
    VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&ba};
    VkFormat format=VK_FORMAT_R8G8B8A8_UNORM;
    uint32_t input_index=0;
    VkRenderingInputAttachmentIndexInfo input={.sType=VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO,
        .colorAttachmentCount=1,.pColorAttachmentInputIndices=&input_index};
    VkCustomResolveCreateInfoEXT custom={.sType=VK_STRUCTURE_TYPE_CUSTOM_RESOLVE_CREATE_INFO_EXT,.pNext=&input,
        .customResolve=is_resolve,.colorAttachmentCount=1,.pColorAttachmentFormats=&format};
    VkPipelineRenderingCreateInfo rendering={.sType=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,.pNext=&custom,
        .viewMask=stereo?3:0,.colorAttachmentCount=1,.pColorAttachmentFormats=&format};
    VkPipelineDepthStencilStateCreateInfo ds={.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,.depthTestEnable=with_depth && !is_resolve,.depthWriteEnable=with_depth && !is_resolve,.depthCompareOp=VK_COMPARE_OP_ALWAYS};
    rendering.depthAttachmentFormat=with_depth?VK_FORMAT_D32_SFLOAT:VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.pNext=dynamic?&rendering:NULL,
        .stageCount=2,.pStages=stages,.pVertexInputState=&vi,.pInputAssemblyState=&ia,.pViewportState=&vp,
        .pRasterizationState=&rs,.pMultisampleState=&ms,.pColorBlendState=&cb,.pDepthStencilState=&ds,.layout=layout,.renderPass=pass,.subpass=subpass};
    VkPipeline result; CHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&ci,NULL,&result)); return result;
}
static void barrier_aspect(VkCommandBuffer cmd,Image a,VkImageAspectFlags aspect,VkImageLayout old,VkImageLayout next,
                    VkAccessFlags src,VkAccessFlags dst,VkPipelineStageFlags s,VkPipelineStageFlags d) {
    VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.srcAccessMask=src,.dstAccessMask=dst,
        .oldLayout=old,.newLayout=next,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=a.image,.subresourceRange={aspect,0,1,0,stereo?2:1}};
    vkCmdPipelineBarrier(cmd,s,d,0,0,NULL,0,NULL,1,&b);
}
static void barrier(VkCommandBuffer cmd,Image a,VkImageLayout old,VkImageLayout next,VkAccessFlags src,VkAccessFlags dst,VkPipelineStageFlags s,VkPipelineStageFlags d) {
    barrier_aspect(cmd,a,VK_IMAGE_ASPECT_COLOR_BIT,old,next,src,dst,s,d);
}
static int run(int dynamic,int custom,int samples,int discard) {
    Image src=image(samples,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT);
    Image dst=image(VK_SAMPLE_COUNT_1_BIT,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    Image depth_src={0},depth_dst={0};
    if(with_depth) {
        depth_src=image_format(samples,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,VK_FORMAT_D32_SFLOAT,VK_IMAGE_ASPECT_DEPTH_BIT);
        depth_dst=image_format(1,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,VK_FORMAT_D32_SFLOAT,VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    VkDescriptorPoolSize ps={VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,1};
    VkDescriptorPoolCreateInfo pc={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1,.poolSizeCount=1,.pPoolSizes=&ps};
    VkDescriptorPool dp; CHECK(vkCreateDescriptorPool(device,&pc,NULL,&dp));
    VkDescriptorSetAllocateInfo sa={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=dp,.descriptorSetCount=1,.pSetLayouts=&set_layout};
    VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(device,&sa,&set));
    VkDescriptorImageInfo ii={.imageView=src.view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write={.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.descriptorCount=1,.descriptorType=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,.pImageInfo=&ii};
    vkUpdateDescriptorSets(device,1,&write,0,NULL);
    VkBufferCreateInfo bc={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=SIZE*SIZE*4*(with_depth?2:1)*(stereo?2:1),.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buffer; CHECK(vkCreateBuffer(device,&bc,NULL,&buffer));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(device,buffer,&req);
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=req.size,
        .memoryTypeIndex=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory bm; CHECK(vkAllocateMemory(device,&ai,NULL,&bm)); CHECK(vkBindBufferMemory(device,buffer,bm,0));
    VkRenderPass pass=VK_NULL_HANDLE; VkFramebuffer fb=VK_NULL_HANDLE;
    if(!dynamic) {
        VkAttachmentDescription att[2]={
            {.format=VK_FORMAT_R8G8B8A8_UNORM,.samples=samples,.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,
             .storeOp=discard?VK_ATTACHMENT_STORE_OP_DONT_CARE:VK_ATTACHMENT_STORE_OP_STORE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_GENERAL},
            {.format=VK_FORMAT_R8G8B8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,
             .storeOp=VK_ATTACHMENT_STORE_OP_STORE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_GENERAL}};
        VkAttachmentReference a0={0,VK_IMAGE_LAYOUT_GENERAL},a1={1,VK_IMAGE_LAYOUT_GENERAL};
        VkSubpassDescription sub[2]={
            {.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&a0},
            {.flags=custom?VK_SUBPASS_DESCRIPTION_CUSTOM_RESOLVE_BIT_EXT:0,.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
             .inputAttachmentCount=1,.pInputAttachments=&a0,.colorAttachmentCount=1,.pColorAttachments=&a1}};
        VkSubpassDependency dep={.srcSubpass=0,.dstSubpass=1,.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask=VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,.dependencyFlags=VK_DEPENDENCY_BY_REGION_BIT};
        VkRenderPassCreateInfo rc={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=2,.pAttachments=att,.subpassCount=2,.pSubpasses=sub,.dependencyCount=1,.pDependencies=&dep};
        CHECK(vkCreateRenderPass(device,&rc,NULL,&pass));
        VkImageView views[2]={src.view,dst.view};
        VkFramebufferCreateInfo fc={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=pass,.attachmentCount=2,.pAttachments=views,.width=SIZE,.height=SIZE,.layers=1};
        CHECK(vkCreateFramebuffer(device,&fc,NULL,&fb));
    }
    VkPipeline p0=pipeline(pass,0,dynamic,0,samples),p1=pipeline(pass,1,dynamic,1,samples);
    VkCommandBufferAllocateInfo ca={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(device,&ca,&cmd));
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cmd,&begin));
    VkClearValue clear={.color={{0.9,0.1,0.1,1.0}}};
    if(dynamic) {
        barrier(cmd,src,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        barrier(cmd,dst,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        if(with_depth) {
            barrier_aspect(cmd,depth_src,VK_IMAGE_ASPECT_DEPTH_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
            barrier_aspect(cmd,depth_dst,VK_IMAGE_ASPECT_DEPTH_BIT,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,0,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkClearDepthStencilValue sentinel={.depth=0.875f};
            VkImageSubresourceRange range={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,stereo?2:1};
            vkCmdClearDepthStencilImage(cmd,depth_dst.image,VK_IMAGE_LAYOUT_GENERAL,&sentinel,1,&range);
            barrier_aspect(cmd,depth_dst,VK_IMAGE_ASPECT_DEPTH_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        }
        uint32_t zero=0;
        VkRenderingInputAttachmentIndexInfo input={.sType=VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO,.colorAttachmentCount=1,.pColorAttachmentInputIndices=&zero};
        VkRenderingAttachmentInfo color={.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=src.view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL,
            .resolveMode=VK_RESOLVE_MODE_CUSTOM_BIT_EXT,.resolveImageView=dst.view,.resolveImageLayout=VK_IMAGE_LAYOUT_GENERAL,
            .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=discard?VK_ATTACHMENT_STORE_OP_DONT_CARE:VK_ATTACHMENT_STORE_OP_STORE,.clearValue=clear};
        VkRenderingAttachmentInfo depth={.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=depth_src.view,.imageLayout=VK_IMAGE_LAYOUT_GENERAL,
            .resolveMode=VK_RESOLVE_MODE_SAMPLE_ZERO_BIT,.resolveImageView=depth_dst.view,.resolveImageLayout=VK_IMAGE_LAYOUT_GENERAL,
            .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=discard?VK_ATTACHMENT_STORE_OP_DONT_CARE:VK_ATTACHMENT_STORE_OP_STORE,.clearValue={.depthStencil={0.9f,0}}};
        VkRenderingInfo ri={.sType=VK_STRUCTURE_TYPE_RENDERING_INFO,.pNext=&input,.flags=VK_RENDERING_CUSTOM_RESOLVE_BIT_EXT|(suspended?VK_RENDERING_SUSPENDING_BIT:0),
            .renderArea={{0,0},{SIZE,SIZE}},.layerCount=1,.viewMask=stereo?3:0,.colorAttachmentCount=1,.pColorAttachments=&color,.pDepthAttachment=with_depth?&depth:NULL};
        vkCmdBeginRendering(cmd,&ri);
        if(suspended) {
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,p0); vkCmdDraw(cmd,3,1,0,0);
            vkCmdEndRendering(cmd);
            ri.flags=VK_RENDERING_CUSTOM_RESOLVE_BIT_EXT|VK_RENDERING_RESUMING_BIT;
            vkCmdBeginRendering(cmd,&ri);
        }
    } else {
        VkRenderPassBeginInfo ri={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=pass,.framebuffer=fb,
            .renderArea={{0,0},{SIZE,SIZE}},.clearValueCount=1,.pClearValues=&clear};
        vkCmdBeginRenderPass(cmd,&ri,VK_SUBPASS_CONTENTS_INLINE);
    }
    if(!suspended || !dynamic) {vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,p0); vkCmdDraw(cmd,3,1,0,0);}
    if(dynamic) {
        VkMemoryBarrier mb={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_INPUT_ATTACHMENT_READ_BIT};
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_DEPENDENCY_BY_REGION_BIT,1,&mb,0,NULL,0,NULL);
        PFN_vkCmdBeginCustomResolveEXT start=(PFN_vkCmdBeginCustomResolveEXT)vkGetDeviceProcAddr(device,"vkCmdBeginCustomResolveEXT");
        if(!start) exit(2); start(cmd,NULL);
    } else vkCmdNextSubpass(cmd,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,p1);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,NULL); vkCmdDraw(cmd,3,1,0,0);
    if(dynamic) vkCmdEndRendering(cmd); else vkCmdEndRenderPass(cmd);
    barrier(cmd,dst,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,stereo?2:1},.imageExtent={SIZE,SIZE,1}};
    vkCmdCopyImageToBuffer(cmd,dst.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);
    if(with_depth) {
        barrier_aspect(cmd,depth_dst,VK_IMAGE_ASPECT_DEPTH_BIT,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        copy.bufferOffset=SIZE*SIZE*4*(stereo?2:1); copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
        vkCmdCopyImageToBuffer(cmd,depth_dst.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer,1,&copy);
    }
    VkMemoryBarrier host={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,NULL,0,NULL);
    CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
    CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
    unsigned char *pixels; CHECK(vkMapMemory(device,bm,0,VK_WHOLE_SIZE,0,(void**)&pixels));
    int bad=0; const int expected[4]={191,64,128,255};
    for(int i=0;i<SIZE*SIZE*(stereo?2:1);i++) for(int c=0;c<4;c++) if(abs((int)pixels[i*4+c]-(c==1 && i>=SIZE*SIZE?128:expected[c]))>1) {bad++; break;}
    printf("%s custom=%d samples=%d store=%s pixels_bad=%d/%d first=%u,%u,%u,%u %s\n",
        dynamic?"dynamic":"legacy",custom,samples,discard?"DONT_CARE":"STORE",bad,SIZE*SIZE*(stereo?2:1),pixels[0],pixels[1],pixels[2],pixels[3],bad?"FAIL":"PASS"); fflush(stdout);
    if(with_depth) {
        float *depth=(float*)(pixels+SIZE*SIZE*4*(stereo?2:1)); int depth_bad=0;
        for(int i=0;i<SIZE*SIZE*(stereo?2:1);i++) if(!(depth[i]>0.37499f && depth[i]<0.37501f)) depth_bad++;
        printf("fixed depth alongside custom color samples=%d layers=%d suspended=%d store=%s pixels_bad=%d/%d first=%.6f expected=0.375000 %s\n",samples,stereo?2:1,suspended,discard?"DONT_CARE":"STORE",depth_bad,SIZE*SIZE*(stereo?2:1),depth[0],depth_bad?"FAIL":"PASS");fflush(stdout);
        bad+=depth_bad;
    }
    vkUnmapMemory(device,bm); vkFreeCommandBuffers(device,pool,1,&cmd);
    vkDestroyPipeline(device,p0,NULL); vkDestroyPipeline(device,p1,NULL);
    if(fb) vkDestroyFramebuffer(device,fb,NULL); if(pass) vkDestroyRenderPass(device,pass,NULL);
    vkDestroyDescriptorPool(device,dp,NULL); vkDestroyBuffer(device,buffer,NULL); vkFreeMemory(device,bm,NULL);
    destroy_image(src); destroy_image(dst); if(with_depth) {destroy_image(depth_src);destroy_image(depth_dst);} return !!bad;
}
int main(int argc,char **argv) {
    if(argc<2 || argc>3) {fprintf(stderr,"usage: %s SHADER_DIRECTORY [KLEPTON_SHIM_DYLIB]\n",argv[0]);return 2;}
    void *library=dlopen(argc==3?argv[2]:"/opt/homebrew/lib/libvulkan.dylib",RTLD_LAZY|RTLD_LOCAL);
    if(!library) {fprintf(stderr,"%s\n",dlerror());return 2;}
    if(argc==3) {
        void *(*symbol)(const char*)=dlsym(library,"kl_vulkan_sym"); if(!symbol) return 2;
        vkGetInstanceProcAddr=symbol("vkGetInstanceProcAddr");
    } else vkGetInstanceProcAddr=dlsym(library,"vkGetInstanceProcAddr");
    if(!vkGetInstanceProcAddr) return 2;
    vkCreateInstance=(PFN_vkCreateInstance)vkGetInstanceProcAddr(VK_NULL_HANDLE,"vkCreateInstance");
    if(!vkCreateInstance) return 2;
    const char *ie[]={VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME};
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="owned-custom-resolve-probe",.apiVersion=VK_API_VERSION_1_3};
    VkInstanceCreateInfo ic={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.flags=VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,.pApplicationInfo=&app,.enabledExtensionCount=1,.ppEnabledExtensionNames=ie};
    VkInstance instance; CHECK(vkCreateInstance(&ic,NULL,&instance)); load_dispatch(instance);
    vkCmdClearDepthStencilImage=(PFN_vkCmdClearDepthStencilImage)vkGetInstanceProcAddr(instance,"vkCmdClearDepthStencilImage");
    uint32_t n=1; VkPhysicalDevice pd; CHECK(vkEnumeratePhysicalDevices(instance,&n,&pd));
    VkPhysicalDeviceDriverProperties driver={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 props={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&driver}; vkGetPhysicalDeviceProperties2(pd,&props);
    printf("device=%s vendor=%#x deviceID=%#x driverVersion=%u driver=%s info=%s\n",props.properties.deviceName,props.properties.vendorID,props.properties.deviceID,props.properties.driverVersion,driver.driverName,driver.driverInfo);
    uint32_t count=0; CHECK(vkEnumerateDeviceExtensionProperties(pd,NULL,&count,NULL));
    VkExtensionProperties *ext=calloc(count,sizeof(*ext)); CHECK(vkEnumerateDeviceExtensionProperties(pd,NULL,&count,ext));
    const char *de[3]; unsigned nd=0;
    for(uint32_t i=0;i<count;i++) {
        const char *e=ext[i].extensionName;
        if(!strcmp(e,VK_EXT_CUSTOM_RESOLVE_EXTENSION_NAME)) {custom_available=1; de[nd++]=VK_EXT_CUSTOM_RESOLVE_EXTENSION_NAME;}
        if(!strcmp(e,"VK_KHR_portability_subset")) de[nd++]="VK_KHR_portability_subset";
        if(!strcmp(e,VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME)) de[nd++]=VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME;
    }
    free(ext); for(unsigned i=0;i<nd;i++) printf("enabled-extension=%s\n",de[i]);
    uint32_t qn=0; vkGetPhysicalDeviceQueueFamilyProperties(pd,&qn,NULL);
    VkQueueFamilyProperties *qp=calloc(qn,sizeof(*qp)); vkGetPhysicalDeviceQueueFamilyProperties(pd,&qn,qp); uint32_t qi=0;
    while(qi<qn && !(qp[qi].queueFlags&VK_QUEUE_GRAPHICS_BIT)) qi++; free(qp); if(qi==qn) return 2;
    vkGetPhysicalDeviceMemoryProperties(pd,&memory);
    VkPhysicalDeviceDynamicRenderingLocalReadFeatures local={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES,.dynamicRenderingLocalRead=1};
    VkPhysicalDeviceCustomResolveFeaturesEXT cr={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_RESOLVE_FEATURES_EXT,.pNext=&local,.customResolve=1};
    VkPhysicalDeviceVulkan11Features f11={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,.pNext=custom_available?&cr:NULL,.multiview=1};
    VkPhysicalDeviceVulkan13Features f13={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,.pNext=&f11,.dynamicRendering=1};
    float priority=1; VkDeviceQueueCreateInfo qc={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=qi,.queueCount=1,.pQueuePriorities=&priority};
    VkDeviceCreateInfo dc={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&f13,.queueCreateInfoCount=1,.pQueueCreateInfos=&qc,.enabledExtensionCount=nd,.ppEnabledExtensionNames=de};
    CHECK(vkCreateDevice(pd,&dc,NULL,&device)); vkGetDeviceQueue(device,qi,0,&queue);
    VkDescriptorSetLayoutBinding binding={.descriptorType=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,.descriptorCount=1,.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo sl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=1,.pBindings=&binding};
    CHECK(vkCreateDescriptorSetLayout(device,&sl,NULL,&set_layout));
    VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,.pSetLayouts=&set_layout}; CHECK(vkCreatePipelineLayout(device,&pl,NULL,&layout));
    VkCommandPoolCreateInfo cp={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=qi}; CHECK(vkCreateCommandPool(device,&cp,NULL,&pool));
    vert=shader(argv[1],"fullscreen.vert"); fill=shader(argv[1],"fill.frag"); resolve[0]=shader(argv[1],"resolve1.frag"); resolve[1]=shader(argv[1],"resolve4.frag"); resolve[2]=shader(argv[1],"resolve2.frag");
    int failures=0;
    for(int s=1;s<=4;s+=3) for(int discard=0;discard<2;discard++) {
        failures+=run(0,0,s,discard);
        if(custom_available) {
            failures+=run(0,1,s,discard);
            // Dynamic resolve sources must be multisampled (VUID 06861).
            if(s>1) failures+=run(1,1,s,discard);
        }
    }
    if(custom_available) {with_depth=1;for(suspended=0;suspended<2;suspended++) for(stereo=0;stereo<2;stereo++) for(int samples=2;samples<=4;samples+=2) for(int discard=0;discard<2;discard++) failures+=run(1,1,samples,discard);}
    if(!custom_available) puts("SKIP custom resolve: extension unavailable");
    vkDestroyShaderModule(device,vert,NULL); vkDestroyShaderModule(device,fill,NULL); vkDestroyShaderModule(device,resolve[0],NULL); vkDestroyShaderModule(device,resolve[1],NULL); vkDestroyShaderModule(device,resolve[2],NULL);
    vkDestroyCommandPool(device,pool,NULL); vkDestroyPipelineLayout(device,layout,NULL); vkDestroyDescriptorSetLayout(device,set_layout,NULL);
    vkDestroyDevice(device,NULL); vkDestroyInstance(instance,NULL); return !!failures;
}
