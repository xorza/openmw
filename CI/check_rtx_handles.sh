#!/bin/bash -e
# Asks that every Vulkan handle the backend owns is owned by `Rtx::Owned`.
#
# **The rule is mechanical, and the holdouts accumulated silently.** `owned.hpp` says it is "the one
# place `vkDestroyX(device, handle, allocator)` is spelled", and a class that spelled it itself paid
# a destructor, a null check and a `const Device&` member that existed so the destructor could reach
# the device. Nothing made the next class adopt the type, so this is what does.
#
# These calls are allowed, and each is allowed for a reason a grep cannot see:
#
#   vkDestroyInstance, vkDestroyDevice         take no parent handle at all, so `Owned`'s
#                                              `Destroy(device, handle, allocator)` has no shape
#                                              for them.
#   vkDestroySurfaceKHR                        takes the instance rather than the device. Two sites
#   vkDestroyDebugUtilsMessengerEXT            is not worth a second template parameter on the
#     (as `mDestroyMessenger`, the pointer      twenty-odd that do take the device.
#      the instance loaded for it)
#   graveyard.cpp                              destroys handles it was *given*, which is the
#                                              counterpart of `Owned::release`: "Hands the handle
#                                              over undestroyed, for a caller that buries it
#                                              instead."
#   accelerationstructure.cpp                  destroys through `mDestroyAccelerationStructure`,
#                                              a pointer the device loaded: `Owned`'s template
#                                              argument names a function the header declares, and
#                                              this one is not. It is the `Owned` for that handle.
#
# A loaded destroyer is matched as well as a declared one, because the first of them stood in
# three classes for a year with this gate reporting that every handle was held.
#
# Another belongs in this list only with its own reason beside it.
#
#   CI/check_rtx_handles.sh

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Comments are stripped before the match, because `owned.hpp`'s own prose and half a dozen others
# name these calls to explain them.
found=$(
    grep -rn --include='*.cpp' --include='*.hpp' -E '(vk|m)Destroy[A-Za-z]+[[:space:]]*\(' \
        "$root/components/rtxvulkan" \
        | grep -v '/owned\.hpp:' \
        | grep -v '/graveyard\.cpp:' \
        | grep -v '/accelerationstructure\.cpp:' \
        | sed -E 's@[[:space:]]*//.*$@@' \
        | grep -E '(vk|m)Destroy[A-Za-z]+[[:space:]]*\(' \
        | grep -Ev '(vkDestroy(Instance|Device|SurfaceKHR|DebugUtilsMessengerEXT)|mDestroyMessenger)[[:space:]]*\(' \
        || true
)

if [[ -n "$found" ]]; then
    echo "a Vulkan handle is destroyed by hand where Rtx::Owned would do it:" >&2
    echo "$found" >&2
    echo >&2
    echo "hold it as Owned<Handle, vkDestroyX> and delete the destructor, or add the call to the" >&2
    echo "exemptions at the top of this script with the reason it cannot be one." >&2
    exit 1
fi

echo "every device-parented Vulkan handle in components/rtxvulkan is held by Rtx::Owned"
