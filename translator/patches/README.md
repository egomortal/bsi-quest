# Local patches (never upstream)

## dxvk-bsi-zero-copy.patch  (against /home/claude/tools/dxvk, DXVK 3.1.1 + BSI Android commits)
Zero-copy D3D11 Map for the recompiled 32-bit guest (step 3, OVERNIGHT.md). Host-visible memory that DXVK maps
persistently is placed inside the guest's 4 GB window, so the runtime's Map returns `mapPtr - M` directly.
- Enables VK_EXT_map_memory_placed (+ memoryMapPlaced, memoryUnmapReserve), VK_KHR_map_memory2 and
  VK_EXT_external_memory_host when the driver has them (harmless otherwise).
- Active only when the runtime sets `BSI_GVA_HOOK="<alloc>,<free>"` (hex host function pointers; bridge.c does this
  when BSI_ZC is on). Without it, DXVK behaves exactly as before.
- Mode 1 (placed, Turnip): vkMapMemory2KHR(VK_MEMORY_MAP_PLACED_BIT_EXT) at a range from `bsi_gva_alloc`; unmapped
  with VK_MEMORY_UNMAP_RESERVE_BIT_EXT before vkFreeMemory, then the range goes back via `bsi_gva_free`.
  Any failure falls back to a normal vkMapMemory (the runtime then uses its shadow path for those maps).
- Mode 2 (host-pointer import, lavapipe = cloud test path): mapped-pool chunks and large non-dedicated mapped
  allocations import guest-window pages (VkImportMemoryHostPointerInfoEXT). `BSI_ZC_MODE=host` forces mode 2.
- Log line at device creation: `BSI zero-copy guest mappings: placed | host-pointer import | off`.
Apply: `cd /home/claude/tools/dxvk && git apply /home/claude/bsi/recomp/patches/dxvk-bsi-zero-copy.patch`, then
`ninja -C build-native` (cloud, copy the .so.0.30101 files to /home/claude/bsi/dxvk_lib/) and
`ninja -C build-android src/d3d11/libdxvk_d3d11.so src/dxgi/libdxvk_dxgi.so` (Quest; the APK packs these two).

## Turnip (Mesa): no patch needed
Checked against Mesa 26.2.4 source (Ubuntu archive tarball; GitHub/GitLab are blocked from the cloud box):
`tu_device.cc` exposes EXT_map_memory_placed with memoryMapPlaced = memoryUnmapReserve = true and
minPlacedMemoryMapAlignment = page size; `tu_knl_kgsl.cc: kgsl_bo_map(dev, bo, placed_addr)` mmaps the KGSL BO with
MAP_FIXED at the placed address (both the gpumem-id and the dma-buf path); `tu_bo_unmap(..., reserve)` replaces the
mapping with a PROT_NONE anonymous reservation. Ordinary vkAllocateMemory BOs are not pre-mapped on KGSL (only
capture-replay BOs, which DXVK does not request), so placement works for DXVK's chunks. Our Turnip build
(own-mesa-main-69745e7) is a 2025+ main build and should have the same code; if the device log says
`placed map ... failed, mapping normally`, check `tu_bo_map` in that tree first.
