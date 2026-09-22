# Open issues

- A renderer made with no window enables `VK_KHR_present_id` without `VK_KHR_swapchain`, which that extension requires; `vkCreateDevice` raises `VUID-vkCreateDevice-ppEnabledExtensionNames-01387` for the headless renderer every GPU test run makes.
