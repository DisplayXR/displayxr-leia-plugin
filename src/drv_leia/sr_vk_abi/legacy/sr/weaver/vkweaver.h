/*!
 * Copyright (C) 2025 Leia, Inc.
 */

#pragma once

#include "sr/management/srcontext.h"
#include "sr/weaver/Weaver.h"
#include "sr/weaver/WeaverTypes.h"
#include "sr/weaver/IWeaverBase.h"
#include <vulkan/vulkan.hpp>

#ifdef WIN32
#   ifdef COMPILING_DLL_SimulatedRealityVulkan
#     define DIMENCOSR_API __declspec(dllexport)
#   else
#     define DIMENCOSR_API __declspec(dllimport)
#   endif
#else
#   define DIMENCOSR_API
#endif

#if defined(__GNUC__) || defined(__GNUG__)
#define DEPRECATED __attribute__((deprecated))
#elif defined(_MSC_VER)
#define DEPRECATED __declspec(deprecated)
#else
#define DEPRECATED
#endif

namespace SR {
    /*!
     * \brief class to be used for weaving in Vulkan applications
     *
     * \ingroup API
     */
    class IVulkanWeaver1 : public virtual IDestroyable, public virtual IQueryInterface, public virtual IWeaverBase1
    {
    public:

        /*!
         * \brief Sets the stereo view texture that will be used for weaving.
         * \param texture Pointer to the texture
         * \param width Width of the texture
         * \param height Height of the texture
         * \param format Pixel format of the texture
         */
        virtual void setInputViewTexture(VkImageView textureViewLeft, VkImageView textureViewRight, int width, int height, VkFormat format) = 0;

        /*!
         * \brief Sets the format of the buffer to which the weaver renders to.
         */
        virtual void setOutputFrameBuffer(VkFramebuffer framebuffer, int width, int height, VkFormat format) = 0;

        /*!
         * \brief Set the commandBuffer to add the weaving commands to. This is required before calling weave().
         */
        virtual void setCommandBuffer(VkCommandBuffer commandBuffer) = 0;

        /*!
         * \brief Set the viewport to use for the weaving commands. This is required before calling weave().
         */
        virtual void setViewport(RECT viewport) = 0;

        /*!
         * \brief Set the scissor rect to use for the weaving commands. This is required before calling weave().
         */
        virtual void setScissorRect(RECT scissorRect) = 0;

    protected:
        IVulkanWeaver1() = default;
        virtual ~IVulkanWeaver1() = 0;

        IVulkanWeaver1(const IVulkanWeaver1&) = delete;
        IVulkanWeaver1& operator=(const IVulkanWeaver1&) = delete;
        IVulkanWeaver1(IVulkanWeaver1&&) = delete;
        IVulkanWeaver1& operator=(IVulkanWeaver1&&) = delete;
    };

    /*!
     * \brief Creates a new Vulkan weaver.
     *
     * \param Context to connect to
     * \param window Handle to (optional) output window
     * \param weaver Pointer to created weaver, if successful
     * \returns WeaverErrorCode Success or error code
     */
    DIMENCOSR_API WeaverErrorCode CreateVulkanWeaver(SRContext& context, VkDevice device, VkPhysicalDevice physicalDevice, VkQueue graphicsQueue, VkCommandPool commandPool, HWND window, IVulkanWeaver1** weaver);
};

#undef DIMENCOSR_API
