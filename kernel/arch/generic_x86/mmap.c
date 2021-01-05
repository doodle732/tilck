/* SPDX-License-Identifier: BSD-2-Clause */

#include <tilck/kernel/system_mmap.h>
#include <tilck/kernel/system_mmap_int.h>

void arch_add_initial_mem_regions()
{
   /* We want to keep the first 64 KB as reserved */
   append_mem_region((struct mem_region) {
      .addr = 0,
      .len = 64 * KB,
      .type = MULTIBOOT_MEMORY_RESERVED,
      .extra = MEM_REG_EXTRA_LOWMEM,
   });

   /*
    * Because we don't mmap regions not explicitly declared as AVAILABLE, we
    * miss some regions in the lower 1 MB. ACPI need to access them. Therefore,
    * create a fake 1 MB wide region marked as "available". Of course, it will
    * be overriden by the real system mem regions, but parts of it will remain.
    */
   append_mem_region((struct mem_region) {
      .addr = 0,
      .len = 1024 * KB,
      .type = MULTIBOOT_MEMORY_AVAILABLE,
      .extra = MEM_REG_EXTRA_LOWMEM,
   });
}

bool arch_add_final_mem_regions()
{
   bool need_sort = false;

   for (int i = 0; i < mem_regions_count; i++) {

      struct mem_region *m = mem_regions + i;

      if (m->type != MULTIBOOT_MEMORY_AVAILABLE || m->extra || m->addr > 16*MB)
         continue;

      /*
       * We found a mem region that:
       *   - is available
       *   - begins in the first 16 MB
       *   - has no extra flags
       */

      if (m->addr + m->len <= 16 * MB) {

         /* The whole region ends in the first 16 MB, just mark it as DMA */
         m->extra |= MEM_REG_EXTRA_DMA;

      } else {

         /*
          * The region ends AFTER the first 16 MB.
          *
          *  +--------------------------------------------+
          *  |                  Region                    |
          *  +--------------------------------------------+
          *  +----------------------------+
          *  |        Usable by DMA       |
          *  +----------------------------+
          *
          * In this case we're going to add a new DMA region and shrink the
          * current one:
          *
          *                               +---------------+
          *                               |    Region     |
          *                               +---------------+
          *  +----------------------------+
          *  |        Usable by DMA       |
          *  +----------------------------+
          *
          */

         append_mem_region((struct mem_region) {
            .addr = m->addr,
            .len = 16 * MB - m->addr,
            .type = MULTIBOOT_MEMORY_AVAILABLE,
            .extra = MEM_REG_EXTRA_DMA,
         });

         m->len = m->addr + m->len - 16 * MB;
         m->addr = 16 * MB;

         need_sort = true;
      }
   }

   return need_sort;
}
