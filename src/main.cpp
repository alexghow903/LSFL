// x11_vulkan_capture.cpp
// Capture an X11 window via XCompositeNameWindowPixmap and present it via Vulkan.
//
// NOTE:
//  - Assumes 32bpp XImage and VK_FORMAT_B8G8R8A8_UNORM swapchain.
//  - No swapchain recreation on resize / OUT_OF_DATE, kept simple for clarity.

#define VK_USE_PLATFORM_XLIB_KHR

#include <vulkan/vulkan.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h> 
#include <X11/Xatom.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/shape.h>


#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <iostream>
#include <algorithm>
#include <unistd.h>

#include <ffx_api/ffx_api.hpp>
#include <ffx_api/ffx_upscale.hpp>
#include <ffx_api/vk/ffx_api_vk.hpp>


static void fatal(const char* msg) {
    std::fprintf(stderr, "Fatal: %s\n", msg);
    std::exit(EXIT_FAILURE);
}

static void vk_check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "Vulkan error %d at %s\n", r, what);
        std::exit(EXIT_FAILURE);
    }
}

/* ----------------------- X11 + XComposite ----------------------- */

struct X11Context {
    Display* dpy = nullptr;
    int screen = 0;
    Window root = 0;
    Window mainWindow = 0;
    Window vkWindow = 0;       // Vulkan-presented window
    Window targetWindow = 0;   // Window we capture
    Pixmap targetPixmap = 0;
    int width = 0;
    int height = 0;
};

void make_fullscreen(X11Context& xc);
void setup_focus_on_target(X11Context& xc);

Window getToplevelFocus(Display* dpy, Window w)
{
    if (!w || w == None) return None;

    Window root = DefaultRootWindow(dpy);
    Window parent = 0;
    Window* children = nullptr;
    unsigned int nchildren = 0;

    Window current = w;

    while (true) {
        Window root_ret, parent_ret;
        Window* children_ret = nullptr;
        unsigned int nchildren_ret = 0;

        if (!XQueryTree(dpy, current, &root_ret, &parent_ret,
                        &children_ret, &nchildren_ret)) {
            break; // query failed, bail out
        }

        if (children_ret)
            XFree(children_ret);

        // If parent is root or none, current is top-level
        if (parent_ret == root || parent_ret == None) {
            return current;
        }

        // Climb one level up
        current = parent_ret;
    }

    return w; // fallback
}


Window getActiveWindow(Display* dpy) {
    Atom prop = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", True);
    if (!prop) return None;

    Atom type;
    int format;
    unsigned long nitems, bytes_after;
    unsigned char* data = nullptr;

    if (XGetWindowProperty(dpy, DefaultRootWindow(dpy), prop,
                           0, 1, False, AnyPropertyType,
                           &type, &format, &nitems, &bytes_after, &data) != Success || !data) {
        return None;
    }

    Window w = *(Window*)data;
    XFree(data);
    return w;
}

Window getFocus(X11Context& xc) {
    int revert_to;
    Window focused = None;
    XGetInputFocus(xc.dpy, &focused, &revert_to);

    if (focused == None || focused == PointerRoot) {
        focused = getActiveWindow(xc.dpy);
    }
    if (focused == None) return None;

    return getToplevelFocus(xc.dpy, focused);
}

void init_x11_main(X11Context& xc) 
{
    xc.dpy = XOpenDisplay(nullptr);
    if (!xc.dpy) fatal("XOpenDisplay failed");

    xc.screen = DefaultScreen(xc.dpy);
    xc.root = RootWindow(xc.dpy, xc.screen);

    xc.mainWindow = XCreateSimpleWindow(xc.dpy, xc.root, 0, 0, 400, 300, 0, BlackPixel(xc.dpy, xc.screen), WhitePixel(xc.dpy, xc.screen));
    XSelectInput(xc.dpy, xc.mainWindow, ExposureMask | StructureNotifyMask);
    XMapWindow(xc.dpy, xc.mainWindow);
    XFlush(xc.dpy);
}

void init_x11_copy(X11Context& xc)
{
    xc.targetWindow = getFocus(xc);

    // Check for XComposite
    int eventBase, errorBase;
    if (!XCompositeQueryExtension(xc.dpy, &eventBase, &errorBase)) {
        fatal("XComposite extension not available");
    }

    int major, minor;
    XCompositeQueryVersion(xc.dpy, &major, &minor);
    std::printf("XComposite version %d.%d\n", major, minor);

    // Query target window size
    XWindowAttributes attrs;
    if (!XGetWindowAttributes(xc.dpy, xc.targetWindow, &attrs)) {
        fatal("XGetWindowAttributes failed");
    }

    xc.width  = attrs.width;
    xc.height = attrs.height;

    XCompositeRedirectWindow(xc.dpy, xc.targetWindow, CompositeRedirectAutomatic);
    XSync(xc.dpy, False); // make errors happen here, not later

    // Name the window's pixmap. On a composited desktop, this refers to the
    // off-screen storage used by the compositor.
    xc.targetPixmap = XCompositeNameWindowPixmap(xc.dpy, xc.targetWindow);
    if (!xc.targetPixmap) {
        fatal("XCompositeNameWindowPixmap returned 0");
    }

    // Create an output window for Vulkan to present into
    XSetWindowAttributes a{};
    a.override_redirect = True;  // <- key: WM won't manage/focus it
    a.event_mask = ExposureMask | StructureNotifyMask;
    a.background_pixel = 0;
    a.border_pixel = 0;

    xc.vkWindow = XCreateWindow(
        xc.dpy, xc.root,
        0, 0, xc.width, xc.height,
        0,
        CopyFromParent,
        InputOutput,
        CopyFromParent,
        CWOverrideRedirect | CWEventMask | CWBackPixel | CWBorderPixel,
        &a
    );

    // XWMHints *h = XAllocWMHints();
    // h->flags = InputHint;
    // h->input = False;
    // XSetWMHints(xc.dpy, xc.vkWindow, h);
    // XFree(h);

    // 2) Make window "click-through" (empty input region)
    XserverRegion empty = XFixesCreateRegion(xc.dpy, nullptr, 0);
    XFixesSetWindowShapeRegion(xc.dpy, xc.vkWindow, ShapeInput, 0, 0, empty);
    XFixesDestroyRegion(xc.dpy, empty);

    // XSelectInput(xc.dpy, xc.vkWindow,
    //          ExposureMask |
    //          StructureNotifyMask);
    XMapWindow(xc.dpy, xc.vkWindow);
    XFlush(xc.dpy);
    // make_fullscreen(xc);
    setup_focus_on_target(xc);
    // int resK = XGrabKeyboard(
    //     xc.dpy,
    //     xc.vkWindow,
    //     True,
    //     GrabModeAsync,
    //     GrabModeAsync,
    //     CurrentTime
    // );
    // if (resK != GrabSuccess)
    //     std::fprintf(stderr, "XGrabKeyboard failed (%d)\n", resK);

    // int resP = XGrabPointer(
    //     xc.dpy,
    //     xc.vkWindow,
    //     True,
    //     ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
    //     GrabModeAsync,
    //     GrabModeAsync,
    //     None,
    //     None,
    //     CurrentTime
    // );
    // if (resP != GrabSuccess)
    //     std::fprintf(stderr, "XGrabPointer failed (%d)\n", resP);
}

/* ---------------------------- Vulkan ---------------------------- */

struct VulkanContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex = 0;
    VkQueue queue = VK_NULL_HANDLE;

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D swapExtent{0,0};
    std::vector<VkImage> swapImages;

    VkCommandPool cmdPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmdBuffers;

    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkSemaphore renderFinished = VK_NULL_HANDLE;
    VkFence inFlight = VK_NULL_HANDLE;

    // Staging buffer for upload
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    VkDeviceSize stagingSize = 0;

    VkExtent2D renderExtent;   // low-res input to FSR
    VkExtent2D displayExtent;  // swapchain / window size

    // NEW: off-screen input color image at render resolution
    VkImage        inputColorImage = VK_NULL_HANDLE;
    VkDeviceMemory inputColorMemory = VK_NULL_HANDLE;
    VkImageView    inputColorView = VK_NULL_HANDLE;

    // NEW: motion-vector image (R16G16_SFLOAT)
    VkImage        motionVectorImage = VK_NULL_HANDLE;
    VkDeviceMemory motionVectorMemory = VK_NULL_HANDLE;
    VkImageView    motionVectorView = VK_NULL_HANDLE;

    // FSR 3 context
    ffx::Context fsrUpscaleContext{};
};

uint32_t findMemoryType(
    VkPhysicalDevice phys,
    uint32_t typeFilter,
    VkMemoryPropertyFlags properties)
{
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(phys, &memProps);

    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }

    fatal("Failed to find suitable memory type");
    return 0;
}

void create_instance(VulkanContext& vc)
{
    const char* extensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_XLIB_SURFACE_EXTENSION_NAME
    };

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "X11 Capture Vulkan";
    app.applicationVersion = VK_MAKE_VERSION(1,0,0);
    app.pEngineName = "NoEngine";
    app.engineVersion = VK_MAKE_VERSION(1,0,0);
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = extensions;

    vk_check(vkCreateInstance(&ci, nullptr, &vc.instance), "vkCreateInstance");
}

void create_xlib_surface(VulkanContext& vc, const X11Context& xc)
{
    VkXlibSurfaceCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    sci.dpy = xc.dpy;
    sci.window = xc.vkWindow;

    vk_check(vkCreateXlibSurfaceKHR(vc.instance, &sci, nullptr, &vc.surface),
             "vkCreateXlibSurfaceKHR");
}

void pick_physical_device_and_queue(VulkanContext& vc)
{
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(vc.instance, &deviceCount, nullptr);
    if (deviceCount == 0) fatal("No Vulkan physical devices found");

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(vc.instance, &deviceCount, devices.data());

    for (auto d : devices) {
        uint32_t qCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qCount, nullptr);
        std::vector<VkQueueFamilyProperties> props(qCount);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qCount, props.data());

        for (uint32_t i = 0; i < qCount; ++i) {
            if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                VkBool32 presentSupported = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(d, i, vc.surface, &presentSupported);
                if (presentSupported) {
                    vc.physDevice = d;
                    vc.queueFamilyIndex = i;
                    return;
                }
            }
        }
    }

    fatal("Failed to find a physical device with graphics+present queue");
}

void create_device_and_queue(VulkanContext& vc)
{
    float priority = 1.0f;

    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = vc.queueFamilyIndex;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    const char* extensions[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };

    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = extensions;

    vk_check(vkCreateDevice(vc.physDevice, &ci, nullptr, &vc.device), "vkCreateDevice");
    vkGetDeviceQueue(vc.device, vc.queueFamilyIndex, 0, &vc.queue);
}

void create_swapchain(VulkanContext& vc, int width, int height)
{
    // Surface capabilities
    VkSurfaceCapabilitiesKHR caps{};
    vk_check(
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vc.physDevice, vc.surface, &caps),
        "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"
    ); 

    // Surface formats
    uint32_t formatCount = 0;
    vk_check(
        vkGetPhysicalDeviceSurfaceFormatsKHR(vc.physDevice, vc.surface, &formatCount, nullptr),
        "vkGetPhysicalDeviceSurfaceFormatsKHR count"
    );
    if (formatCount == 0) fatal("No surface formats available");

    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vk_check(
        vkGetPhysicalDeviceSurfaceFormatsKHR(vc.physDevice, vc.surface,
                                             &formatCount, formats.data()),
        "vkGetPhysicalDeviceSurfaceFormatsKHR"
    );

    VkSurfaceFormatKHR chosenFormat = formats[0];
    if (formatCount == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
        chosenFormat.format = VK_FORMAT_B8G8R8A8_UNORM;
        chosenFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    } else {
        for (const auto& f : formats) {
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                chosenFormat = f;
                break;
            }
        }
    }
    vc.swapchainFormat = chosenFormat.format;

    // Present mode: prefer MAILBOX, else FIFO
    uint32_t presentModeCount = 0;
    vk_check(
        vkGetPhysicalDeviceSurfacePresentModesKHR(vc.physDevice, vc.surface,
                                                  &presentModeCount, nullptr),
        "vkGetPhysicalDeviceSurfacePresentModesKHR count"
    );
    std::vector<VkPresentModeKHR> presentModes(presentModeCount);
    vk_check(
        vkGetPhysicalDeviceSurfacePresentModesKHR(vc.physDevice, vc.surface,
                                                  &presentModeCount, presentModes.data()),
        "vkGetPhysicalDeviceSurfacePresentModesKHR"
    );

    VkPresentModeKHR chosenPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (auto pm : presentModes) {
        if (pm == VK_PRESENT_MODE_MAILBOX_KHR) {
            chosenPresentMode = pm;
            break;
        }
    }

    // Extent
    if (caps.currentExtent.width != UINT32_MAX) {
        vc.swapExtent = caps.currentExtent;
    } else {
        VkExtent2D e{};
        e.width  = static_cast<uint32_t>(width);
        e.height = static_cast<uint32_t>(height);
        if (e.width  < caps.minImageExtent.width)  e.width  = caps.minImageExtent.width;
        if (e.height < caps.minImageExtent.height) e.height = caps.minImageExtent.height;
        if (e.width  > caps.maxImageExtent.width)  e.width  = caps.maxImageExtent.width;
        if (e.height > caps.maxImageExtent.height) e.height = caps.maxImageExtent.height;
        vc.swapExtent = e;
    }

    // Image count
    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
        imageCount = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = vc.surface;
    sci.minImageCount = imageCount;
    sci.imageFormat = vc.swapchainFormat;
    sci.imageColorSpace = chosenFormat.colorSpace;
    sci.imageExtent = vc.swapExtent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = chosenPresentMode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = VK_NULL_HANDLE;

    vk_check(vkCreateSwapchainKHR(vc.device, &sci, nullptr, &vc.swapchain),
             "vkCreateSwapchainKHR");

    vk_check(
        vkGetSwapchainImagesKHR(vc.device, vc.swapchain, &imageCount, nullptr),
        "vkGetSwapchainImagesKHR count"
    );
    vc.swapImages.resize(imageCount);
    vk_check(
        vkGetSwapchainImagesKHR(vc.device, vc.swapchain, &imageCount, vc.swapImages.data()),
        "vkGetSwapchainImagesKHR"
    );
}

void create_command_pool_and_buffers(VulkanContext& vc)
{
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = vc.queueFamilyIndex;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    vk_check(vkCreateCommandPool(vc.device, &pci, nullptr, &vc.cmdPool),
             "vkCreateCommandPool");

    vc.cmdBuffers.resize(vc.swapImages.size());

    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = vc.cmdPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = static_cast<uint32_t>(vc.cmdBuffers.size());

    vk_check(vkAllocateCommandBuffers(vc.device, &ai, vc.cmdBuffers.data()),
             "vkAllocateCommandBuffers");
}

void create_sync_objects(VulkanContext& vc)
{
    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    vk_check(vkCreateSemaphore(vc.device, &sci, nullptr, &vc.imageAvailable),
             "vkCreateSemaphore imageAvailable");
    vk_check(vkCreateSemaphore(vc.device, &sci, nullptr, &vc.renderFinished),
             "vkCreateSemaphore renderFinished");

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    vk_check(vkCreateFence(vc.device, &fci, nullptr, &vc.inFlight),
             "vkCreateFence inFlight");
}

void create_staging_buffer(VulkanContext& vc)
{
    vc.stagingSize = vc.swapExtent.width * vc.swapExtent.height * 4ull;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = vc.stagingSize;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    vk_check(vkCreateBuffer(vc.device, &bci, nullptr, &vc.stagingBuffer),
             "vkCreateBuffer stagingBuffer");

    VkMemoryRequirements memReq{};
    vkGetBufferMemoryRequirements(vc.device, vc.stagingBuffer, &memReq);

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = memReq.size;
    mai.memoryTypeIndex = findMemoryType(
        vc.physDevice,
        memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    vk_check(vkAllocateMemory(vc.device, &mai, nullptr, &vc.stagingMemory),
             "vkAllocateMemory stagingMemory");
    vk_check(vkBindBufferMemory(vc.device, vc.stagingBuffer, vc.stagingMemory, 0),
             "vkBindBufferMemory stagingBuffer");
}

/* --------- Capture XComposite pixmap into RAM each frame ---------- */

struct CaptureBuffer {
    XImage* image = nullptr;
};

struct FrameHistory {
    CaptureBuffer prev;
    CaptureBuffer curr;
    bool hasPrev = false;
};

bool capture_frame(const X11Context& xc, CaptureBuffer& cb)
{
    if (cb.image) {
        XDestroyImage(cb.image);
        cb.image = nullptr;
    }

    XSync(xc.dpy, False);

    cb.image = XGetImage(
        xc.dpy,
        xc.targetPixmap,
        0, 0,
        xc.width, xc.height,
        AllPlanes,
        ZPixmap
    );

    if (!cb.image) {
        std::fprintf(stderr, "XGetImage failed\n");
        return false;
    }
    if (cb.image->bits_per_pixel != 32) {
        std::fprintf(stderr,
                     "Only 32bpp XImage supported (got %d)\n",
                     cb.image->bits_per_pixel);
        return false;
    }

    return true;
}



/* --------- Upload capture buffer into staging buffer (CPU) -------- */

void upload_capture_to_staging(
    const X11Context& xc,
    const CaptureBuffer& cb,
    VulkanContext& vc)
{
    void* mapped = nullptr;
    vk_check(
        vkMapMemory(vc.device, vc.stagingMemory, 0, vc.stagingSize, 0, &mapped),
        "vkMapMemory staging"
    );

    auto* dst = static_cast<std::uint8_t*>(mapped);
    auto* src = reinterpret_cast<std::uint8_t*>(cb.image->data);

    const int srcStride = cb.image->bytes_per_line;

    // Clamp to whichever is smaller: captured image or swapchain
    const int width  = std::min<int>(vc.swapExtent.width,  cb.image->width);
    const int height = std::min<int>(vc.swapExtent.height, cb.image->height);

    const int dstStride      = static_cast<int>(vc.swapExtent.width) * 4;
    const int copyWidthBytes = width * 4;

    for (int y = 0; y < height; ++y) {
        std::memcpy(
            dst + y * dstStride,
            src + y * srcStride,
            copyWidthBytes
        );
    }

    vkUnmapMemory(vc.device, vc.stagingMemory);
}

/* --------- Record copy from staging buffer to swapchain image -------- */

void record_copy_to_swap_image(
    VulkanContext& vc,
    uint32_t imageIndex)
{
    VkCommandBuffer cmd = vc.cmdBuffers[imageIndex];

    vk_check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vk_check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");

    VkImage swapImg = vc.swapImages[imageIndex];

    // Layout transition: UNDEFINED -> TRANSFER_DST_OPTIMAL
    VkImageMemoryBarrier toTransfer{};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = swapImg;
    toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransfer.subresourceRange.baseMipLevel = 0;
    toTransfer.subresourceRange.levelCount = 1;
    toTransfer.subresourceRange.baseArrayLayer = 0;
    toTransfer.subresourceRange.layerCount = 1;
    toTransfer.srcAccessMask = 0;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &toTransfer
    );

    // Copy buffer -> image
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;   // tightly packed, use imageExtent.width
    region.bufferImageHeight = 0; // tightly packed
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = { vc.swapExtent.width, vc.swapExtent.height, 1 };

    vkCmdCopyBufferToImage(
        cmd,
        vc.stagingBuffer,
        swapImg,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &region
    );

    // Layout transition: TRANSFER_DST_OPTIMAL -> PRESENT_SRC_KHR
    VkImageMemoryBarrier toPresent{};
    toPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.image = swapImg;
    toPresent.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toPresent.subresourceRange.baseMipLevel = 0;
    toPresent.subresourceRange.levelCount = 1;
    toPresent.subresourceRange.baseArrayLayer = 0;
    toPresent.subresourceRange.layerCount = 1;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toPresent.dstAccessMask = 0;

    vkCmdPipelineBarrier(
        cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &toPresent
    );

    vk_check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
}

void recreate_swapchain(VulkanContext& vc, X11Context& xc)
{
    // Wait until GPU is idle before tearing things down
    vkDeviceWaitIdle(vc.device);

    // Destroy / free resources tied to swapchain extent
    if (!vc.cmdBuffers.empty()) {
        vkFreeCommandBuffers(
            vc.device,
            vc.cmdPool,
            static_cast<uint32_t>(vc.cmdBuffers.size()),
            vc.cmdBuffers.data()
        );
        vc.cmdBuffers.clear();
    }

    if (vc.stagingBuffer) {
        vkDestroyBuffer(vc.device, vc.stagingBuffer, nullptr);
        vc.stagingBuffer = VK_NULL_HANDLE;
    }
    if (vc.stagingMemory) {
        vkFreeMemory(vc.device, vc.stagingMemory, nullptr);
        vc.stagingMemory = VK_NULL_HANDLE;
    }

    if (vc.swapchain) {
        vkDestroySwapchainKHR(vc.device, vc.swapchain, nullptr);
        vc.swapchain = VK_NULL_HANDLE;
    }

    // Ask X11 what the new window size is
    XWindowAttributes attrs;
    if (!XGetWindowAttributes(xc.dpy, xc.vkWindow, &attrs)) {
        fatal("XGetWindowAttributes failed in recreate_swapchain");
    }
    xc.width  = attrs.width;
    xc.height = attrs.height;

    // Rebuild swapchain + dependent resources at new size
    create_swapchain(vc, xc.width, xc.height);     // updates vc.swapExtent
    create_command_pool_and_buffers(vc);
    create_staging_buffer(vc);
}

void update_target_pixmap_if_needed(X11Context& xc)
{
    XWindowAttributes attrs;
    if (!XGetWindowAttributes(xc.dpy, xc.targetWindow, &attrs)) {
        std::fprintf(stderr, "XGetWindowAttributes on targetWindow failed\n");
        return;
    }

    int newW = attrs.width;
    int newH = attrs.height;

    // No change – nothing to do
    if (newW == xc.width && newH == xc.height) {
        return;
    }

    std::printf("Source window resized: %dx%d -> %dx%d\n",
                xc.width, xc.height, newW, newH);

    xc.width  = newW;
    xc.height = newH;

    // Drop the old named pixmap (it will no longer be updated by the server)
    if (xc.targetPixmap) {
        XFreePixmap(xc.dpy, xc.targetPixmap);
        xc.targetPixmap = 0;
    }

    // Name the new backing pixmap for the resized window
    xc.targetPixmap = XCompositeNameWindowPixmap(xc.dpy, xc.targetWindow);
    if (!xc.targetPixmap) {
        std::fprintf(stderr, "XCompositeNameWindowPixmap after resize returned 0\n");
    }
}

void make_fullscreen(X11Context& xc)
{
    // Resize to cover the whole screen
    int sw = DisplayWidth(xc.dpy, xc.screen);
    int sh = DisplayHeight(xc.dpy, xc.screen);
    XMoveResizeWindow(xc.dpy, xc.vkWindow, 0, 0, sw, sh);

    Atom wm_state   = XInternAtom(xc.dpy, "_NET_WM_STATE", False);
    Atom fullscreen = XInternAtom(xc.dpy, "_NET_WM_STATE_FULLSCREEN", False);

    XEvent xev;
    std::memset(&xev, 0, sizeof(xev));
    xev.type                 = ClientMessage;
    xev.xclient.window       = xc.vkWindow;
    xev.xclient.message_type = wm_state;
    xev.xclient.format       = 32;
    xev.xclient.data.l[0]    = 1;          // _NET_WM_STATE_ADD
    xev.xclient.data.l[1]    = fullscreen; // first property to add
    xev.xclient.data.l[2]    = 0;          // second property (none)
    xev.xclient.data.l[3]    = 1;          // normal app
    xev.xclient.data.l[4]    = 0;

    XSendEvent(
        xc.dpy,
        DefaultRootWindow(xc.dpy),
        False,
        SubstructureRedirectMask | SubstructureNotifyMask,
        &xev
    );
}

// void forward_key_to_target(X11Context& xc, XKeyEvent* kev)
// {
//     Bool is_press = (kev->type == KeyPress);

//     XTestFakeKeyEvent(
//         xc.dpy,
//         kev->keycode,
//         is_press,
//         CurrentTime
//     );

//     XFlush(xc.dpy);
// }

// void forward_button_to_target(X11Context& xc, XButtonEvent* bev)
// {
//     XWarpPointer(
//         xc.dpy,
//         None,
//         xc.targetWindow,
//         0, 0, 0, 0,
//         bev->x,
//         bev->y
//     );

//     Bool is_press = (bev->type == ButtonPress);

//     XTestFakeButtonEvent(
//         xc.dpy,
//         bev->button,
//         is_press,
//         CurrentTime
//     );

//     XFlush(xc.dpy);
// }

// void forward_motion_to_target(X11Context& xc, XMotionEvent* mev)
// {
//     XWarpPointer(
//         xc.dpy,
//         None,
//         xc.targetWindow,
//         0, 0, 0, 0,
//         mev->x,
//         mev->y
//     );

//     XFlush(xc.dpy);
// }

void setup_focus_on_target(X11Context& xc)
{
    // Give keyboard focus to the source window
    XSetInputFocus(xc.dpy, xc.targetWindow, RevertToParent, CurrentTime);
}
/* ------------------------------ Cleanup ------------------------------ */

void cleanup(VulkanContext& vc, X11Context& xc, CaptureBuffer& cb)
{
    if (vc.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(vc.device);

        if (vc.stagingBuffer) vkDestroyBuffer(vc.device, vc.stagingBuffer, nullptr);
        if (vc.stagingMemory) vkFreeMemory(vc.device, vc.stagingMemory, nullptr);

        if (vc.imageAvailable) vkDestroySemaphore(vc.device, vc.imageAvailable, nullptr);
        if (vc.renderFinished) vkDestroySemaphore(vc.device, vc.renderFinished, nullptr);
        if (vc.inFlight) vkDestroyFence(vc.device, vc.inFlight, nullptr);

        if (vc.cmdPool) vkDestroyCommandPool(vc.device, vc.cmdPool, nullptr);
        if (vc.swapchain) vkDestroySwapchainKHR(vc.device, vc.swapchain, nullptr);
        if (vc.surface) vkDestroySurfaceKHR(vc.instance, vc.surface, nullptr);

        vkDestroyDevice(vc.device, nullptr);
    }

    if (vc.instance != VK_NULL_HANDLE) {
        vkDestroyInstance(vc.instance, nullptr);
    }

    if (cb.image) {
        XDestroyImage(cb.image);
        cb.image = nullptr;
    }

    if (xc.targetPixmap) {
        XFreePixmap(xc.dpy, xc.targetPixmap);
        xc.targetPixmap = 0;
    }
    if (xc.vkWindow) {
        XDestroyWindow(xc.dpy, xc.vkWindow);
        xc.vkWindow = 0;
    }

    if (xc.dpy) {
        XCloseDisplay(xc.dpy);
        xc.dpy = nullptr;
    }
}

void main_loop(X11Context& xc)
{
    init_x11_copy(xc);
    VulkanContext vc{};
    create_instance(vc);
    create_xlib_surface(vc, xc);
    pick_physical_device_and_queue(vc);
    create_device_and_queue(vc);
    create_swapchain(vc, xc.width, xc.height);
    create_command_pool_and_buffers(vc);
    create_sync_objects(vc);
    create_staging_buffer(vc);

    CaptureBuffer capture{};

    bool running = true;
    while (running) {
        // Basic X11 event loop
        while (XPending(xc.dpy)) {
            XEvent ev;
            XNextEvent(xc.dpy, &ev);
            switch (ev.type) {
            case DestroyNotify:
                running = false;
                break;

            // case KeyPress:
            // case KeyRelease:
            //     forward_key_to_target(xc, &ev.xkey);
            //     break;

            // case ButtonPress:
            // case ButtonRelease:
            //     forward_button_to_target(xc, &ev.xbutton);
            //     break;

            // case MotionNotify:
            //     forward_motion_to_target(xc, &ev.xmotion);
            //     break;

            case ConfigureNotify:
                recreate_swapchain(vc, xc);
                break;

            default:
                break;
            }
        }

        update_target_pixmap_if_needed(xc);

        if (!capture_frame(xc, capture)) {
            // If capture fails, just continue; this may happen if window is minimized, etc.
            continue;
        }

        upload_capture_to_staging(xc, capture, vc);

        vk_check(
            vkWaitForFences(vc.device, 1, &vc.inFlight, VK_TRUE, UINT64_MAX),
            "vkWaitForFences"
        );
        vk_check(vkResetFences(vc.device, 1, &vc.inFlight), "vkResetFences");

        uint32_t imageIndex = 0;
        VkResult acquire = vkAcquireNextImageKHR(
            vc.device,
            vc.swapchain,
            UINT64_MAX,
            vc.imageAvailable,
            VK_NULL_HANDLE,
            &imageIndex
        );

        if (acquire == VK_ERROR_OUT_OF_DATE_KHR || acquire == VK_SUBOPTIMAL_KHR) {
            recreate_swapchain(vc, xc);
            // Skip this frame; next loop iteration will use the new swapchain
            continue;
        } else if (acquire != VK_SUCCESS) {
            std::fprintf(stderr,
                        "vkAcquireNextImageKHR error %d; exiting.\n", acquire);
            break;
        }

        record_copy_to_swap_image(vc, imageIndex);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &vc.imageAvailable;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &vc.cmdBuffers[imageIndex];
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &vc.renderFinished;

        vk_check(vkQueueSubmit(vc.queue, 1, &submit, vc.inFlight), "vkQueueSubmit");

        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &vc.renderFinished;
        present.swapchainCount = 1;
        present.pSwapchains = &vc.swapchain;
        present.pImageIndices = &imageIndex;

        VkResult presRes = vkQueuePresentKHR(vc.queue, &present);
        if (presRes == VK_ERROR_OUT_OF_DATE_KHR || presRes == VK_SUBOPTIMAL_KHR) {
            recreate_swapchain(vc, xc);
            continue; // next frame will pick up the new swapchain
        } else if (presRes != VK_SUCCESS) {
            std::fprintf(stderr,
                        "vkQueuePresentKHR error %d; exiting.\n", presRes);
            break;
        }
    }

    cleanup(vc, xc, capture);
}

/* ------------------------------ Main ------------------------------ */

int main()
{
    sleep(3);
    X11Context xc{};
    init_x11_main(xc);
    main_loop(xc);
    return 0;
}
