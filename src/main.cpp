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
#include <X11/keysym.h>


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
#include <chrono>

#include <ffx_api/ffx_api.hpp>
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.hpp>
#include <ffx_api/vk/ffx_api_vk.hpp>
#include <ffx_api/ffx_upscale.hpp>


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

    // Capture (source window) size
    int capW = 0;
    int capH = 0;

    // Output (fullscreen) size
    int outW = 0;
    int outH = 0;
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

    xc.capW = attrs.width;
    xc.capH = attrs.height;

    // Output size (fullscreen)
    xc.outW = DisplayWidth(xc.dpy, xc.screen);
    xc.outH = DisplayHeight(xc.dpy, xc.screen);

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
        0, 0, xc.outW, xc.outH,
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
    make_fullscreen(xc);
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
    VkExtent2D captureExtent{0,0};

    // NEW: off-screen input color image at render resolution
    VkImage        inputColorImage = VK_NULL_HANDLE;
    VkDeviceMemory inputColorMemory = VK_NULL_HANDLE;
    VkImageView    inputColorView = VK_NULL_HANDLE;

    // NEW: motion-vector image (R16G16_SFLOAT)
    VkImage        motionVectorImage = VK_NULL_HANDLE;
    VkDeviceMemory motionVectorMemory = VK_NULL_HANDLE;
    VkImageView    motionVectorView = VK_NULL_HANDLE;

    // Add to VulkanContext (next to your existing images)
    VkImage        outputColorImage = VK_NULL_HANDLE;
    VkDeviceMemory outputColorMemory = VK_NULL_HANDLE;
    VkImageView    outputColorView = VK_NULL_HANDLE;

    VkImage        depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    VkImageView    depthView = VK_NULL_HANDLE;

    VkImage        captureColorImage  = VK_NULL_HANDLE;
    VkDeviceMemory captureColorMemory = VK_NULL_HANDLE;
    VkImageView    captureColorView = VK_NULL_HANDLE;
};

struct FSRContext {
    ffx::CreateBackendVKDesc backendDesc{};
    ffx::CreateContextDescUpscale createFsr{};
    ffx::ReturnCode retCodeCreate;
    ffx::Context m_UpscalingContext = nullptr;

    ffx::DispatchDescUpscale dispatchUpscale{};
    ffx::ReturnCode retCodeDispatch;
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
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
        VK_KHR_BIND_MEMORY_2_EXTENSION_NAME
    };

    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = 4;
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
    vc.stagingSize = (VkDeviceSize)vc.captureExtent.width * vc.captureExtent.height * 4ull;

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
        xc.capW, xc.capH,
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

    // Clear whole staging to black (prevents garbage borders)
    std::memset(dst, 0, (size_t)vc.stagingSize);

    const int srcStride = cb.image->bytes_per_line;

    const int width  = std::min<int>((int)vc.captureExtent.width,  cb.image->width);
    const int height = std::min<int>((int)vc.captureExtent.height, cb.image->height);

    const int dstStride      = (int)vc.captureExtent.width * 4;
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


// 1. Create an image with memory
void create_image(
    VulkanContext& vc,
    uint32_t width,
    uint32_t height,
    VkFormat format,
    VkImageUsageFlags usage,
    VkImage& image,
    VkDeviceMemory& memory)
{
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    vk_check(vkCreateImage(vc.device, &ici, nullptr, &image), "vkCreateImage");

    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(vc.device, image, &memReq);

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = memReq.size;
    mai.memoryTypeIndex = findMemoryType(
        vc.physDevice,
        memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    );

    vk_check(vkAllocateMemory(vc.device, &mai, nullptr, &memory), "vkAllocateMemory");
    vk_check(vkBindImageMemory(vc.device, image, memory, 0), "vkBindImageMemory");
}

// 2. Create image view
VkImageView create_image_view(
    VulkanContext& vc,
    VkImage image,
    VkFormat format,
    VkImageAspectFlags aspectFlags)
{
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange.aspectMask = aspectFlags;
    vci.subresourceRange.baseMipLevel = 0;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.baseArrayLayer = 0;
    vci.subresourceRange.layerCount = 1;

    VkImageView view = VK_NULL_HANDLE;
    vk_check(vkCreateImageView(vc.device, &vci, nullptr, &view), "vkCreateImageView");
    return view;
}

// 3. Create all FSR-required images
void create_fsr_images(VulkanContext& vc)
{
    // Input color image (low-res captured content)
    create_image(
        vc,
        vc.renderExtent.width,
        vc.renderExtent.height,
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        vc.inputColorImage,
        vc.inputColorMemory
    );
    vc.inputColorView = create_image_view(
        vc, vc.inputColorImage, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT
    );

    // Output color image (upscaled result)
    create_image(
        vc,
        vc.displayExtent.width,
        vc.displayExtent.height,
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        vc.outputColorImage,
        vc.outputColorMemory
    );
    vc.outputColorView = create_image_view(
        vc, vc.outputColorImage, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT
    );

    // Motion vectors (optional but improves quality)
    create_image(
        vc,
        vc.renderExtent.width,
        vc.renderExtent.height,
        VK_FORMAT_R16G16_SFLOAT,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        vc.motionVectorImage,
        vc.motionVectorMemory
    );
    vc.motionVectorView = create_image_view(
        vc, vc.motionVectorImage, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT
    );

    // Depth buffer (optional)
    create_image(
        vc,
        vc.renderExtent.width,
        vc.renderExtent.height,
        VK_FORMAT_D32_SFLOAT,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        vc.depthImage,
        vc.depthMemory
    );
    vc.depthView = create_image_view(
        vc, vc.depthImage, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT
    );

    create_image(
        vc,
        vc.captureExtent.width,
        vc.captureExtent.height,
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        vc.captureColorImage,
        vc.captureColorMemory
    );
}

// 4. Initialize FSR context properly
void initFSR(VulkanContext& vc, FSRContext& fc) 
{
    fc.backendDesc = {};
    fc.createFsr   = {};

    // IMPORTANT: context must be null on CreateContext
    fc.m_UpscalingContext = nullptr;

    fc.backendDesc.header.type     = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    fc.backendDesc.vkDevice        = vc.device;
    fc.backendDesc.vkPhysicalDevice= vc.physDevice;
    fc.backendDesc.vkDeviceProcAddr= vkGetDeviceProcAddr;

    fc.createFsr.header.type       = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    fc.createFsr.maxUpscaleSize    = { vc.displayExtent.width, vc.displayExtent.height };
    fc.createFsr.maxRenderSize     = { vc.renderExtent.width,  vc.renderExtent.height  };

    // Highly recommended while bringing it up:
    fc.createFsr.flags = FFX_UPSCALE_ENABLE_DEBUG_CHECKING; // + whatever else you need
    // fc.createFsr.fpMessage = &YourFfxMsgCallback;         // to see WHY it fails

    fc.retCodeCreate = ffx::CreateContext(fc.m_UpscalingContext, nullptr, fc.createFsr, fc.backendDesc);
    if (!fc.retCodeCreate || !fc.m_UpscalingContext) {
    fprintf(stderr, "CreateContext failed: %d\n", (int)fc.retCodeCreate);
    return;
}
}

// 5. Transition image layout helper
void transition_image_layout(
    VkCommandBuffer cmd,
    VkImage image,
    VkImageLayout oldLayout,
    VkImageLayout newLayout,
    VkImageAspectFlags aspectMask)
{
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspectMask;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags srcStage, dstStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && 
        newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && 
               newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && 
               newLayout == VK_IMAGE_LAYOUT_GENERAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
        newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL &&
            newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

    } else if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
            newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT; // good enough for your pipeline
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = 0;
        srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }

    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// 6. Dispatch FSR upscaling
static FfxApiSurfaceFormat vk_to_ffx_surface_format(VkFormat fmt) {
    switch (fmt) {
        case VK_FORMAT_B8G8R8A8_UNORM:   return FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_R16G16_SFLOAT:    return FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
        case VK_FORMAT_D32_SFLOAT:       return FFX_API_SURFACE_FORMAT_R32_FLOAT; // depth is treated as single-channel float
        default:                         return FFX_API_SURFACE_FORMAT_UNKNOWN;
    }
}

// NOTE: FidelityFX API needs a fully described resource on Vulkan (VkImage doesn't carry format/size at runtime).
// The Vulkan backend header (ffx_api_vk.hpp/.h) provides the helper function used here.
// If your SDK version uses a different helper name/signature, adjust ONLY this function.
static FfxApiResource make_ffx_api_resource_vk(
    VkImage image,
    VkImageView imageView,
    VkFormat format,
    uint32_t width,
    uint32_t height,
    FfxApiResourceState state,
    const char* name,
    uint32_t additionalUsages = 0
) {
    FfxApiResourceDescription desc{};
    desc.type     = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    desc.format   = vk_to_ffx_surface_format(format);
    desc.width    = width;
    desc.height   = height;
    desc.depth    = 1;
    desc.mipCount = 1;
    desc.flags    = 0;
    desc.usage    = additionalUsages;


    return ffxApiGetResourceVK(image, desc, state);

}

void dispatch_fsr(VulkanContext& vc, FSRContext& fc, VkCommandBuffer cmd, float jitterX, float jitterY, float deltaTime)
{
    if (!fc.m_UpscalingContext) return;

    // Build the dispatch struct described in the FSR3 upscaling docs.
    // (jitter sign is typically NEGATED vs what you applied to the camera)
    fc.dispatchUpscale = {}; // reset to defaults each frame

    fc.dispatchUpscale.commandList = cmd;

    // Inputs (render resolution)
    fc.dispatchUpscale.color = make_ffx_api_resource_vk(
        vc.inputColorImage, vc.inputColorView, VK_FORMAT_B8G8R8A8_UNORM,
        vc.renderExtent.width, vc.renderExtent.height,
        FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ,
        "LS_InputColor"
    );

    fc.dispatchUpscale.depth = make_ffx_api_resource_vk(
        vc.depthImage, vc.depthView, VK_FORMAT_D32_SFLOAT,
        vc.renderExtent.width, vc.renderExtent.height,
        FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ,
        "LS_Depth"
    );

    fc.dispatchUpscale.motionVectors = make_ffx_api_resource_vk(
        vc.motionVectorImage, vc.motionVectorView, VK_FORMAT_R16G16_SFLOAT,
        vc.renderExtent.width, vc.renderExtent.height,
        FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ,
        "LS_MotionVectors"
    );

    // Optional inputs: leave empty unless you generate them
    fc.dispatchUpscale.exposure                   = {};
    fc.dispatchUpscale.reactive                   = {};
    fc.dispatchUpscale.transparencyAndComposition = {};

    // Output (presentation resolution). Mark as UAV-capable if your SDK uses usage flags.
    fc.dispatchUpscale.output = make_ffx_api_resource_vk(
        vc.outputColorImage, vc.outputColorView, VK_FORMAT_B8G8R8A8_UNORM,
        vc.displayExtent.width, vc.displayExtent.height,
        FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ,
        "LS_OutputColor",
        FFX_API_RESOURCE_USAGE_UAV
    );

    // Jitter (note the sign convention in the SDK docs)
    fc.dispatchUpscale.jitterOffset.x = -jitterX;
    fc.dispatchUpscale.jitterOffset.y = -jitterY;

    // If motion vectors are in pixels, scale is render resolution (as shown in SDK examples)
    fc.dispatchUpscale.motionVectorScale.x = (float)vc.renderExtent.width;
    fc.dispatchUpscale.motionVectorScale.y = (float)vc.renderExtent.height;

    fc.dispatchUpscale.renderSize  = { vc.renderExtent.width,  vc.renderExtent.height };
    fc.dispatchUpscale.upscaleSize = { vc.displayExtent.width, vc.displayExtent.height };

    // Sharpening (optional)
    fc.dispatchUpscale.enableSharpening = false;
    fc.dispatchUpscale.sharpness        = 0.0f;

    // SDK expects milliseconds
    fc.dispatchUpscale.frameTimeDelta = deltaTime * 1000.0f;

    // If you don't have real camera/depth info, these are best-effort placeholders.
    fc.dispatchUpscale.preExposure             = 1.0f;
    static bool s_firstFrame = true;
    fc.dispatchUpscale.reset                   = s_firstFrame;
    fc.dispatchUpscale.cameraNear              = 0.1f;
    fc.dispatchUpscale.cameraFar               = 1000.0f;
    fc.dispatchUpscale.cameraFovAngleVertical  = 1.0f;   // ~57 degrees
    fc.dispatchUpscale.viewSpaceToMetersFactor = 1.0f;
    fc.dispatchUpscale.flags                   = 0;

    ffx::ReturnCode rc = ffx::Dispatch(fc.m_UpscalingContext, fc.dispatchUpscale);
    if (!rc) {
        fprintf(stderr, "ffx::Dispatch(UPSCALE) failed: %d\n", (int)rc);
    }

    s_firstFrame = false;
}

// 7. Cleanup FSR resources
void cleanup_fsr(VulkanContext& vc, FSRContext& fc)
{
    if (fc.m_UpscalingContext) {
        ffx::DestroyContext(fc.m_UpscalingContext);
        fc.m_UpscalingContext = nullptr;
    }
    
    if (vc.inputColorView) vkDestroyImageView(vc.device, vc.inputColorView, nullptr);
    if (vc.inputColorImage) vkDestroyImage(vc.device, vc.inputColorImage, nullptr);
    if (vc.inputColorMemory) vkFreeMemory(vc.device, vc.inputColorMemory, nullptr);
    
    if (vc.outputColorView) vkDestroyImageView(vc.device, vc.outputColorView, nullptr);
    if (vc.outputColorImage) vkDestroyImage(vc.device, vc.outputColorImage, nullptr);
    if (vc.outputColorMemory) vkFreeMemory(vc.device, vc.outputColorMemory, nullptr);
    
    if (vc.motionVectorView) vkDestroyImageView(vc.device, vc.motionVectorView, nullptr);
    if (vc.motionVectorImage) vkDestroyImage(vc.device, vc.motionVectorImage, nullptr);
    if (vc.motionVectorMemory) vkFreeMemory(vc.device, vc.motionVectorMemory, nullptr);
    
    if (vc.depthView) vkDestroyImageView(vc.device, vc.depthView, nullptr);
    if (vc.depthImage) vkDestroyImage(vc.device, vc.depthImage, nullptr);
    if (vc.depthMemory) vkFreeMemory(vc.device, vc.depthMemory, nullptr);

    if (vc.captureColorImage)  vkDestroyImage(vc.device, vc.captureColorImage, nullptr);
    if (vc.captureColorMemory) vkFreeMemory(vc.device, vc.captureColorMemory, nullptr);
    vc.captureColorImage  = VK_NULL_HANDLE;
    vc.captureColorMemory = VK_NULL_HANDLE;
}

/* --------- Record copy from staging buffer to swapchain image -------- */

// Replace your record_copy_to_swap_image function with this enhanced version
void record_upscale_and_present(
    VulkanContext& vc,
    FSRContext& fc,
    uint32_t imageIndex,
    float deltaTime,
    uint32_t frameCount)
{
    VkCommandBuffer cmd = vc.cmdBuffers[imageIndex];
    vk_check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");

    // STEP 1: Copy captured data from staging buffer to inputColorImage
    VkImageLayout capOld = (frameCount == 0)
        ? VK_IMAGE_LAYOUT_UNDEFINED
        : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    transition_image_layout(
        cmd, vc.captureColorImage,
        capOld,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_ASPECT_COLOR_BIT
    );

    // IMPORTANT: your staging rows are packed as swapExtent.width * 4 bytes per row
    // (see upload_capture_to_staging: dstStride = vc.swapExtent.width * 4) :contentReference[oaicite:6]{index=6}
    VkBufferImageCopy capCopy{};
    capCopy.bufferOffset = 0;
    capCopy.bufferRowLength   = vc.captureExtent.width;
    capCopy.bufferImageHeight = vc.captureExtent.height;
    capCopy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    capCopy.imageSubresource.mipLevel = 0;
    capCopy.imageSubresource.baseArrayLayer = 0;
    capCopy.imageSubresource.layerCount = 1;
    capCopy.imageOffset = {0, 0, 0};
    capCopy.imageExtent = { vc.captureExtent.width, vc.captureExtent.height, 1 };

    vkCmdCopyBufferToImage(
        cmd,
        vc.stagingBuffer,
        vc.captureColorImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1,
        &capCopy
    );

    transition_image_layout(
        cmd, vc.captureColorImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_ASPECT_COLOR_BIT
    );

    // --- Prepare low-res inputColorImage as blit destination ---
    VkImageLayout inOld = (frameCount == 0)
        ? VK_IMAGE_LAYOUT_UNDEFINED
        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    transition_image_layout(
        cmd, vc.inputColorImage,
        inOld,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_ASPECT_COLOR_BIT
    );

    // --- Blit (scale) full-res capture -> low-res input ---
    VkImageBlit blit{};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[0]  = { 0, 0, 0 };
    blit.srcOffsets[1] =  { (int)vc.captureExtent.width, (int)vc.captureExtent.height, 1 };

    blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.dstOffsets[0]  = { 0, 0, 0 };
    blit.dstOffsets[1]  = { (int)vc.renderExtent.width, (int)vc.renderExtent.height, 1 };

    vkCmdBlitImage(
        cmd,
        vc.captureColorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        vc.inputColorImage,   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blit,
        VK_FILTER_NEAREST
    );

    // --- Input is now ready for FSR sampling ---
    transition_image_layout(
        cmd, vc.inputColorImage,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_ASPECT_COLOR_BIT
    );

    // STEP 2: Prepare output image for FSR
    transition_image_layout(
        cmd, vc.outputColorImage,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_ASPECT_COLOR_BIT
    );

    // STEP 3: Run FSR upscaling
    // Simple halton sequence for jitter (improves temporal quality)
    float jitterX = 0.0f, jitterY = 0.0f;
    if (frameCount % 2 == 0) {
        jitterX = 0.5f / vc.renderExtent.width;
        jitterY = 0.5f / vc.renderExtent.height;
    }
    
    dispatch_fsr(vc, fc, cmd, jitterX, jitterY, deltaTime); 

    // STEP 4: Copy upscaled result to swapchain
    VkImage swapImg = vc.swapImages[imageIndex];

    transition_image_layout(
        cmd, swapImg,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_ASPECT_COLOR_BIT
    );

    VkImageCopy copyToSwap{};
    copyToSwap.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyToSwap.srcSubresource.mipLevel = 0;
    copyToSwap.srcSubresource.baseArrayLayer = 0;
    copyToSwap.srcSubresource.layerCount = 1;
    copyToSwap.srcOffset = {0, 0, 0};
    copyToSwap.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyToSwap.dstSubresource.mipLevel = 0;
    copyToSwap.dstSubresource.baseArrayLayer = 0;
    copyToSwap.dstSubresource.layerCount = 1;
    copyToSwap.dstOffset = {0, 0, 0};
    copyToSwap.extent = {vc.displayExtent.width, vc.displayExtent.height, 1};

    vkCmdCopyImage(
        cmd,
        vc.outputColorImage, VK_IMAGE_LAYOUT_GENERAL,
        swapImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &copyToSwap
    );

    transition_image_layout(
        cmd, swapImg,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_IMAGE_ASPECT_COLOR_BIT
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
    xc.outW  = attrs.width;
    xc.outH = attrs.height;

    // Rebuild swapchain + dependent resources at new size
    create_swapchain(vc, xc.outW, xc.outH);     // updates vc.swapExtent
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
    if (newW == xc.capW && newH == xc.capH) {
        return;
    }

    std::printf("Source window resized: %dx%d -> %dx%d\n",
                xc.capW, xc.capH, newW, newH);

    xc.capW = newW;
    xc.capH = newH;

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

void grab_toggle_hotkey(X11Context& xc)
{
    KeyCode keycode = XKeysymToKeycode(xc.dpy, XK_s);
    unsigned int modifiers = ControlMask | Mod1Mask; // Ctrl + Alt

    // Grab with and without NumLock / CapsLock
    const unsigned int locks[] = { 0, LockMask, Mod2Mask, (unsigned)(LockMask | Mod2Mask) };

    for (unsigned int lock : locks) {
        XGrabKey(xc.dpy, keycode, modifiers | lock, xc.root, False, GrabModeAsync, GrabModeAsync);
    }

    XSelectInput(xc.dpy, xc.root, KeyPressMask);
    XFlush(xc.dpy);
}


/* ------------------------------ Cleanup ------------------------------ */

void cleanup_session(VulkanContext& vc, X11Context& xc, CaptureBuffer& cb)
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

    // IMPORTANT: do NOT XCloseDisplay here.
}

void cleanup_app(X11Context& xc)
{
    if (xc.mainWindow) {
        XDestroyWindow(xc.dpy, xc.mainWindow);
        xc.mainWindow = 0;
    }
    if (xc.dpy) {
        XCloseDisplay(xc.dpy);
        xc.dpy = nullptr;
    }
}

static bool is_toggle_hotkey(const XKeyEvent& k)
{
    KeySym sym = XLookupKeysym(const_cast<XKeyEvent*>(&k), 0);
    const unsigned int want = ControlMask | Mod1Mask;
    return sym == XK_s && (k.state & want) == want;
}


bool run_session(X11Context& xc)
{
    init_x11_copy(xc);
    VulkanContext vc{};
    create_instance(vc);
    create_xlib_surface(vc, xc);
    pick_physical_device_and_queue(vc);
    create_device_and_queue(vc);
    
    vc.captureExtent = { (uint32_t)xc.capW, (uint32_t)xc.capH };
    vc.displayExtent = { (uint32_t)xc.outW, (uint32_t)xc.outH };

    // Lossless path: render at capture res (no half-res)
    vc.renderExtent = vc.captureExtent;
    
    create_swapchain(vc, (int)vc.displayExtent.width, (int)vc.displayExtent.height);
    vc.displayExtent = vc.swapExtent;
    create_command_pool_and_buffers(vc);
    create_sync_objects(vc);
    create_staging_buffer(vc);
    
    // Create FSR images and initialize
    create_fsr_images(vc);
    
    FSRContext fc{};
    initFSR(vc, fc);

    CaptureBuffer capture{};

    bool running = true;
    bool app_exit = false;
    
    auto lastTime = std::chrono::high_resolution_clock::now();
    uint32_t frameCount = 0;

    while (running) {
        // Calculate delta time
        auto currentTime = std::chrono::high_resolution_clock::now();
        float deltaTime = std::chrono::duration<float>(currentTime - lastTime).count();
        lastTime = currentTime;
        
        while (XPending(xc.dpy)) {
            XEvent ev;
            XNextEvent(xc.dpy, &ev);

            switch (ev.type) {
            case DestroyNotify:
                if (ev.xdestroywindow.window == xc.mainWindow) {
                    app_exit = true;
                }
                running = false;
                break;

            case KeyPress:
                if (is_toggle_hotkey(ev.xkey)) {
                    running = false;
                }
                break;

            case ConfigureNotify:
                if (ev.xconfigure.window == xc.vkWindow) {
                    // Need to recreate FSR images too
                    vkDeviceWaitIdle(vc.device);
                    cleanup_fsr(vc, fc);
                    recreate_swapchain(vc, xc);
                    
                    vc.displayExtent = { (uint32_t)xc.outW, (uint32_t)xc.outH };
                    vc.renderExtent  = vc.captureExtent; // lossless
                    
                    create_fsr_images(vc);
                    initFSR(vc, fc);
                }
                break;
            }
        }

        if (!running) break;

        update_target_pixmap_if_needed(xc);

        if (!capture_frame(xc, capture)) {
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
            vkDeviceWaitIdle(vc.device);
            cleanup_fsr(vc, fc);
            recreate_swapchain(vc, xc);
            
            vc.displayExtent = { (uint32_t)xc.outW, (uint32_t)xc.outH };
            vc.renderExtent  = vc.captureExtent; // lossless
            
            create_fsr_images(vc);
            initFSR(vc, fc);
            continue;
        } else if (acquire != VK_SUCCESS) {
            std::fprintf(stderr, "vkAcquireNextImageKHR error %d\n", acquire);
            break;
        }

        record_upscale_and_present(vc, fc, imageIndex, deltaTime, frameCount++);

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
            continue;
        } else if (presRes != VK_SUCCESS) {
            std::fprintf(stderr, "vkQueuePresentKHR error %d\n", presRes);
            break;
        }
    }
    cleanup_fsr(vc, fc);
    cleanup_session(vc, xc, capture);
    return app_exit;
}

/* ------------------------------ Main ------------------------------ */

int main()
{
    X11Context xc{};
    xc.dpy = XOpenDisplay(nullptr);
    if (!xc.dpy) fatal("XOpenDisplay failed");
    xc.screen = DefaultScreen(xc.dpy);
    xc.root = RootWindow(xc.dpy, xc.screen);
    
    init_x11_main(xc);

    grab_toggle_hotkey(xc);

    bool app_running = true;

    while (app_running) {
        XEvent ev;
        XNextEvent(xc.dpy, &ev); // blocking wait

        if (ev.type == DestroyNotify && ev.xdestroywindow.window == xc.mainWindow) {
            app_running = false;
            break;
        }

        if (ev.type == KeyPress && is_toggle_hotkey(ev.xkey)) {
            // Start session; it will return when Ctrl+Alt+S is pressed again.
            fprintf(stderr, "KeyPress received in main loop\n");
            bool want_exit = run_session(xc);
            if (want_exit) app_running = false;
        }

        // handle GUI expose/button/etc here if you want
    }

    cleanup_app(xc);
    return 0;
}