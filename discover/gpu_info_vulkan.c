#include "gpu_info_vulkan.h"

#include <string.h>
#include <limits.h>

int check_perfmon(vk_handle_t* rh) {
#ifdef __linux__
  cap_t caps;
  const cap_value_t cap_list[1] = {CAP_PERFMON};

  caps = (*rh->cap_get_proc)();
  if (caps == NULL)
    return -1;

  if ((*rh->cap_set_flag)(caps, CAP_EFFECTIVE, 1, cap_list, CAP_SET) == -1)
    return -1;

  if ((*rh->cap_set_proc)(caps) == -1)
    return -1;

  if ((*rh->cap_free)(caps) == -1)
    return -1;
#endif

  return 0;
}

int is_extension_supported(vk_handle_t* rh, VkPhysicalDevice device, char* extension) {
  VkPhysicalDeviceProperties properties;
  (*rh->vkGetPhysicalDeviceProperties)(device, &properties);

  uint32_t extensionCount;
  (*rh->vkEnumerateDeviceExtensionProperties)(device, NULL, &extensionCount, NULL);

  if (extensionCount == 0) {
    return 0;
  }

  VkExtensionProperties* extensions = malloc(extensionCount * sizeof(VkExtensionProperties));
  if (extensions == NULL) {
    return 0;
  }

  (*rh->vkEnumerateDeviceExtensionProperties)(device, NULL, &extensionCount, extensions);

  for (int j = 0; j < extensionCount; j++) {
    if (strcmp(extensions[j].extensionName, extension) == 0) {
      free(extensions);
      return 1;
    }
  }

  free(extensions);
  return 0;
}

void vk_init(char* vk_lib_path, char* cap_lib_path, vk_init_resp_t *resp) {
  const int buflen = 256;
  char buf[buflen + 1];
  int i;

  struct lookup {
    int is_cap;
    char *s;
    void **p;
  } l[] = {
#ifdef __linux__
      {1, "cap_get_proc", (void *)&resp->ch.cap_get_proc},
      {1, "cap_get_bound", (void *)&resp->ch.cap_get_bound},
      {1, "cap_set_flag", (void *)&resp->ch.cap_set_flag},
      {1, "cap_set_proc", (void *)&resp->ch.cap_set_proc},
      {1, "cap_free", (void *)&resp->ch.cap_free},
#endif
      {0, "vkGetPhysicalDeviceProperties", (void *)&resp->ch.vkGetPhysicalDeviceProperties},
      {0, "vkEnumerateDeviceExtensionProperties", (void *)&resp->ch.vkEnumerateDeviceExtensionProperties},
      {0, "vkGetPhysicalDeviceMemoryProperties2", (void *)&resp->ch.vkGetPhysicalDeviceMemoryProperties2},
      {0, "vkGetPhysicalDeviceQueueFamilyProperties", (void *)&resp->ch.vkGetPhysicalDeviceQueueFamilyProperties},
      {0, "vkAllocateMemory", (void *)&resp->ch.vkAllocateMemory},
      {0, "vkCreateDevice", (void *)&resp->ch.vkCreateDevice},
      {0, "vkCreateInstance", (void *)&resp->ch.vkCreateInstance},
      {0, "vkEnumeratePhysicalDevices", (void *)&resp->ch.vkEnumeratePhysicalDevices},
      {0, "vkFreeMemory", (void *)&resp->ch.vkFreeMemory},
      {0, "vkDestroyDevice", (void *)&resp->ch.vkDestroyDevice},
      {0, "vkDestroyInstance", (void *)&resp->ch.vkDestroyInstance},
      {0, NULL, NULL},
  };

  resp->ch.vk_handle = LOAD_LIBRARY(vk_lib_path, RTLD_LAZY);
  if (!resp->ch.vk_handle) {
    char *msg = LOAD_ERR();
    LOG(resp->ch.verbose, "library %s load err: %s\n", vk_lib_path, msg);
    snprintf(buf, buflen,
            "Unable to load %s library to query for Vulkan GPUs: %s",
            vk_lib_path, msg);
    free(msg);
    resp->err = strdup(buf);
    return;
  }

#ifdef __linux__
  resp->ch.cap_handle = LOAD_LIBRARY(cap_lib_path, RTLD_LAZY);
  if (!resp->ch.cap_handle) {
    char *msg = LOAD_ERR();
    LOG(resp->ch.verbose, "library %s load err: %s\n", cap_lib_path, msg);
    snprintf(buf, buflen,
            "Unable to load %s library to query for Vulkan GPUs: %s",
            cap_lib_path, msg);
    free(msg);
    resp->err = strdup(buf);
    return;
  }
#endif

  for (i = 0; l[i].s != NULL; i++) {
    if (l[i].is_cap)
#ifdef __linux__
      *l[i].p = LOAD_SYMBOL(resp->ch.cap_handle, l[i].s);
#else
      continue;
#endif
    else
      *l[i].p = LOAD_SYMBOL(resp->ch.vk_handle, l[i].s);
    if (!*l[i].p) {
      char *msg = LOAD_ERR();
      LOG(resp->ch.verbose, "dlerr: %s\n", msg);
      if (l[i].is_cap) {
        UNLOAD_LIBRARY(resp->ch.cap_handle);
        resp->ch.cap_handle = NULL;
      } else {
        UNLOAD_LIBRARY(resp->ch.vk_handle);
        resp->ch.vk_handle = NULL;
      }
      snprintf(buf, buflen, "symbol lookup for %s failed: %s", l[i].s,
              msg);
      free(msg);
      resp->err = strdup(buf);
      return;
    }
  }

  if (check_perfmon(&resp->ch) != 0) {
    resp->err = strdup("performance monitoring is not allowed. Please enable CAP_PERFMON or run as root to use Vulkan.");
    LOG(resp->ch.verbose, "vulkan: %s", resp->err);
    return;
  }

  VkInstance instance;

  VkApplicationInfo appInfo = {};
  appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pNext = NULL;
  appInfo.pApplicationName = "Ollama";
  appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
  appInfo.pEngineName = "No Engine";
  appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
  appInfo.apiVersion = VK_API_VERSION_1_2;

  VkInstanceCreateInfo createInfo = {};
  createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  createInfo.pNext = NULL;
  createInfo.flags = 0;
  createInfo.enabledExtensionCount = 1;
  const char* extensions[] = { VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME };
  createInfo.ppEnabledExtensionNames = extensions;
  createInfo.pApplicationInfo = &appInfo;

  VkResult result = (*resp->ch.vkCreateInstance)(&createInfo, NULL, &instance);
  if (result != VK_SUCCESS) {
    resp->err = strdup("failed to create instance");
    return;
  }

  uint32_t deviceCount;
  result = (*resp->ch.vkEnumeratePhysicalDevices)(instance, &deviceCount, NULL);
  if (result != VK_SUCCESS) {
    resp->err = strdup("failed to enumerate physical devices");
    return;
  }

  resp->err = NULL;
  resp->ch.vk = instance;
  resp->ch.num_devices = deviceCount;
  resp->num_devices = deviceCount;
}

int vk_check_flash_attention(vk_handle_t rh, int i) {
  VkInstance instance = rh.vk;
  uint32_t deviceCount = rh.num_devices;

  VkPhysicalDevice* devices = malloc(deviceCount * sizeof(VkPhysicalDevice));
  if (devices == NULL) {
    return 0;
  }

  VkResult result = (*rh.vkEnumeratePhysicalDevices)(instance, &deviceCount, devices);
  if (result != VK_SUCCESS) {
    free(devices);
    return 0;
  }

  VkPhysicalDeviceProperties properties;
  (*rh.vkGetPhysicalDeviceProperties)(devices[i], &properties);

  int supports_nv_coopmat2 = is_extension_supported(&rh, devices[i], VK_NV_COOPERATIVE_MATRIX_2_EXTENSION_NAME);
  if (!supports_nv_coopmat2) {
    free(devices);
    return 1;
  }

  free(devices);
  return 0;
}

// For debug messages
const char* memoryPropertyFlagsToString(VkMemoryPropertyFlags flags) {
  static char buffer[256];
  buffer[0] = '\0';

  if (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
    strcat(buffer, "DEVICE_LOCAL ");
  }
  if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
    strcat(buffer, "HOST_VISIBLE ");
  }
  if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) {
    strcat(buffer, "HOST_COHERENT ");
  }
  if (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) {
    strcat(buffer, "HOST_CACHED ");
  }
  if (flags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) {
    strcat(buffer, "LAZILY_ALLOCATED ");
  }
  if (flags & VK_MEMORY_PROPERTY_PROTECTED_BIT) {
    strcat(buffer, "PROTECTED ");
  }
  if (flags & VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD) {
    strcat(buffer, "DEVICE_COHERENT_AMD ");
  }
  if (flags & VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD) {
    strcat(buffer, "DEVICE_UNCACHED_AMD ");
  }
  if (flags & VK_MEMORY_PROPERTY_RDMA_CAPABLE_BIT_NV) {
    strcat(buffer, "RDMA_CAPABLE_NV ");
  }

  if (buffer[0] != '\0') {
    buffer[strlen(buffer) - 1] = '\0';
  }

  return buffer;
}

// For debug messages
const char* physicalDeviceTypeToString(VkPhysicalDeviceType type) {
  switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_OTHER:
      return "Other";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
      return "Integrated GPU";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
      return "Discrete GPU";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
      return "Virtual GPU";
    default: // Shouldn't reach
      return "Unknown?";
  }
}

// Helper function to count bits in VkMemoryPropertyFlags.
uint32_t countBits(VkMemoryPropertyFlags flags) {
  uint32_t count = 0;
  while (flags) {
    count += flags & 1;
    flags >>= 1;
  }
  return count;
}

void vk_check_vram(vk_handle_t rh, int i, mem_info_t *resp) {
  VkInstance instance = rh.vk;
  uint32_t deviceCount = rh.num_devices;

  VkPhysicalDevice* devices = malloc(deviceCount * sizeof(VkPhysicalDevice));
  if (devices == NULL) {
    resp->err = strdup("memory allocation failed for devices array");
    return;
  }

  VkResult result = (*rh.vkEnumeratePhysicalDevices)(instance, &deviceCount, devices);
  if (result != VK_SUCCESS) {
    free(devices);
    resp->err = strdup("failed to enumerate physical devices");
    return;
  }

  VkPhysicalDeviceProperties properties;
  (*rh.vkGetPhysicalDeviceProperties)(devices[i], &properties);
  LOG(rh.verbose, "Device: %s\n", properties.deviceName);

  int supports_budget = is_extension_supported(&rh, devices[i], VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
  if (!supports_budget) {
    free(devices);
    resp->err = strdup("device does not support memory budget");
    return;
  }

  // CPUs aren't considered
  if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
    free(devices);
    resp->err = strdup("device is a CPU");
    return;
  }

  VkPhysicalDeviceMemoryBudgetPropertiesEXT physical_device_memory_budget_properties;
  physical_device_memory_budget_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
  physical_device_memory_budget_properties.pNext = NULL;

  VkPhysicalDeviceMemoryProperties2 device_memory_properties;
  device_memory_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
  device_memory_properties.pNext = &physical_device_memory_budget_properties;

  // Query device memory information
  (*rh.vkGetPhysicalDeviceMemoryProperties2)(devices[i], &device_memory_properties);

  LOG(rh.verbose, "Device type: %s\n", physicalDeviceTypeToString(properties.deviceType));

  VkPhysicalDeviceMemoryProperties memoryProps = device_memory_properties.memoryProperties;

  // HEURISTIC
  // Find memory type with least amount of flags for each heap
  uint32_t shortestMemoryTypes[memoryProps.memoryHeapCount];

  for (uint32_t heapIndex = 0; heapIndex < memoryProps.memoryHeapCount; heapIndex++) {
    uint32_t bestMemoryTypeIndex = UINT_MAX;
    uint32_t minFlagCount = UINT_MAX;

    // Iterate over all memory types.
    for (uint32_t typeIndex = 0; typeIndex < memoryProps.memoryTypeCount; typeIndex++) {
      // Check if the memory type belongs to the current heap.
      if (memoryProps.memoryTypes[typeIndex].heapIndex == heapIndex) {
        VkMemoryPropertyFlags flags = memoryProps.memoryTypes[typeIndex].propertyFlags;
        uint32_t flagCount = countBits(flags);

        // Use the first valid memory type, or update if this one has fewer bits set.
        if (bestMemoryTypeIndex == UINT_MAX || flagCount < minFlagCount) {
          bestMemoryTypeIndex = typeIndex;
          minFlagCount = flagCount;
        }
      }
    }

    LOG(rh.verbose, "Heap %u: Best memory type index %u with %u flags set (0x%x)\n",
        heapIndex, bestMemoryTypeIndex, minFlagCount, memoryProps.memoryTypes[bestMemoryTypeIndex].propertyFlags);
    shortestMemoryTypes[heapIndex] = bestMemoryTypeIndex;
  }

  VkDeviceSize device_memory_total_size  = 0;
  VkDeviceSize device_memory_heap_budget = 0;

  uint32_t bestHeapIndex = 0; // TODO maybe needs to be an array?
  LOG(rh.verbose, "MemoryHeap count: %u, MemoryType count: %u\n", memoryProps.memoryHeapCount, memoryProps.memoryTypeCount);
  for (uint32_t j = 0; j < memoryProps.memoryHeapCount; j++) {
    VkMemoryHeap heap = device_memory_properties.memoryProperties.memoryHeaps[j];
    uint32_t shortestMemoryType = shortestMemoryTypes[j];
    VkMemoryPropertyFlags shortestFlags = memoryProps.memoryTypes[shortestMemoryType].propertyFlags;

    switch (properties.deviceType) {
      case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        // Only DEVICE_LOCAL heaps, NOT DEVICE_LOCAL & HOST_VISIBLE, those
        // are likely virtual (Re)BAR variants/copies that we shouldn't count.
        // Probably mostly relevant for older GPUs, two examples I know are
        // Nvidia GTX970 and AMD RX580.
        // Should work normally for modern GPUs, this is mostly a workaround for
        // the older ones.
        if ((heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            !(shortestFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT &&
              shortestFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
          device_memory_total_size  += heap.size;
          device_memory_heap_budget += physical_device_memory_budget_properties.heapBudget[j];
          bestHeapIndex = j; // TODO better selection?
        }
        break;
      case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
      case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
      case VK_PHYSICAL_DEVICE_TYPE_OTHER:
        // iGPUs/APUs are often only DEVICE_LOCAL and HOST_VISIBLE and need special handling.
        // This code path should also work fine for VIRTUAL_GPUs, and if they exist, for the
        // OTHER type too.
        if ((heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && (shortestFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
          device_memory_total_size  += heap.size;
          device_memory_heap_budget += physical_device_memory_budget_properties.heapBudget[j];
          bestHeapIndex = j; // TODO better selection?
        }
        break;
      default:
        break;
    }
  }

  if (rh.verbose) {
    for (uint32_t i = 0; i < device_memory_properties.memoryProperties.memoryTypeCount; i++) {
      VkMemoryType memoryType = device_memory_properties.memoryProperties.memoryTypes[i];

      LOG(rh.verbose, "Memory Type %d: Heap Index = %d, Property Flags = %s\n",
          i, memoryType.heapIndex, memoryPropertyFlagsToString(memoryType.propertyFlags));
    }
  }

  // DYNAMIC MEMORY ESTIMATION

  // Get queue families
  uint32_t queueFamilyCount = 0;
  (*rh.vkGetPhysicalDeviceQueueFamilyProperties)(devices[i], &queueFamilyCount, NULL);
  VkQueueFamilyProperties* queueFamilies = malloc(queueFamilyCount * sizeof(VkQueueFamilyProperties));
  (*rh.vkGetPhysicalDeviceQueueFamilyProperties)(devices[i], &queueFamilyCount, queueFamilies);

  // Find compute family
  int computeFamilyIndex = -1;
  for (uint32_t i = 0; i < queueFamilyCount; i++) {
    if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
      computeFamilyIndex = i;
      break;
    }
  }

  free(queueFamilies);

  if (computeFamilyIndex == -1) {
    free(devices);
    resp->err = strdup("failed to find a compute queue family");
    return;
  }

  // Setup VkDevice
  float queuePriority = 1.0f;
  VkDeviceQueueCreateInfo queueCreateInfo = {};
  queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queueCreateInfo.queueFamilyIndex = computeFamilyIndex;
  queueCreateInfo.queueCount = 1;
  queueCreateInfo.pQueuePriorities = &queuePriority;

  VkDeviceCreateInfo createInfo = {};
  createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  createInfo.pQueueCreateInfos = &queueCreateInfo;
  createInfo.queueCreateInfoCount = 1;
  createInfo.pEnabledFeatures = NULL;

  const char* deviceExtensions[] = {};
  createInfo.enabledExtensionCount = sizeof(deviceExtensions) / sizeof(deviceExtensions[0]);
  createInfo.ppEnabledExtensionNames = deviceExtensions;

  VkDevice device;
  result = (*rh.vkCreateDevice)(devices[i], &createInfo, NULL, &device);
  if (result != VK_SUCCESS) {
    free(devices);
    resp->err = strdup("unable to create VkDevice for memory estimation");
    return;
  }

  VkMemoryAllocateInfo allocInfo = {0};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = device_memory_heap_budget;
  allocInfo.memoryTypeIndex = shortestMemoryTypes[bestHeapIndex];

  VkDeviceMemory memory = VK_NULL_HANDLE;
  uint64_t originalSize = allocInfo.allocationSize;
  uint64_t successfulAllocationSize = 0; // NEW VARIABLE TO STORE SUCCESSFUL SIZE

  do {
    result = (*rh.vkAllocateMemory)(device, &allocInfo, NULL, &memory);

    if (result == VK_SUCCESS) {
      successfulAllocationSize = allocInfo.allocationSize; // SAVE SIZE ON SUCCESS
      LOG(rh.verbose, "Allocation of size %zu on heap %d successful!\n",
          (uint64_t) allocInfo.allocationSize, bestHeapIndex);
      (*rh.vkFreeMemory)(device, memory, NULL);
      break;
    } else {
      LOG(rh.verbose, "Allocation of size %zu on heap %d failed.\n",
          (uint64_t) allocInfo.allocationSize, bestHeapIndex);

      // TODO CHANGE TO BINARY SEARCH!
      // Calculate new allocation size (1% reduction)
      uint64_t reduction = originalSize / 100;
      if (reduction == 0) reduction = 1; // Ensure minimum reduction of 1 byte
      allocInfo.allocationSize -= reduction;

      if (allocInfo.allocationSize == 0) {
        LOG(rh.verbose, "Allocation failed completely - no memory available\n");
        break;
      }
    }
  } while (1);

  (*rh.vkDestroyDevice)(device, NULL);

  free(devices);

  resp->err = NULL;
  snprintf(&resp->gpu_id[0], GPU_ID_LEN, "%d", i);
  resp->gpu_name[GPU_NAME_LEN - 1] = '\0';
  strncpy(&resp->gpu_name[0], properties.deviceName, GPU_NAME_LEN - 1);
  resp->total = (uint64_t) device_memory_total_size;
  resp->free = (uint64_t) successfulAllocationSize;
  resp->major = VK_API_VERSION_MAJOR(properties.apiVersion);
  resp->minor = VK_API_VERSION_MINOR(properties.apiVersion);
  resp->patch = VK_API_VERSION_PATCH(properties.apiVersion);
}

void vk_release(vk_handle_t rh) {
  LOG(rh.verbose, "releasing vulkan library\n");
  (*rh.vkDestroyInstance)(rh.vk, NULL);
  UNLOAD_LIBRARY(rh.vk_handle);
  rh.vk_handle = NULL;

#ifdef __linux__
  LOG(rh.verbose, "releasing libcap library\n");
  UNLOAD_LIBRARY(rh.cap_handle);
  rh.cap_handle = NULL;
#endif
}
