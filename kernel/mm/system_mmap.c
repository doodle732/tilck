/* SPDX-License-Identifier: BSD-2-Clause */

#include <tilck/common/basic_defs.h>
#include <tilck/common/string_util.h>
#include <tilck/common/utils.h>

#include <tilck/kernel/system_mmap.h>
#include <tilck/kernel/paging.h>
#include <tilck/kernel/sort.h>
#include <tilck/kernel/elf_utils.h>
#include <tilck/kernel/hal.h>

u32 __mem_lower_kb;
u32 __mem_upper_kb;

memory_region_t mem_regions[MAX_MEM_REGIONS];
int mem_regions_count;

void append_mem_region(memory_region_t r)
{
   if (mem_regions_count >= (int)ARRAY_SIZE(mem_regions))
      panic("Too many memory regions (limit: %u)", ARRAY_SIZE(mem_regions));

   mem_regions[mem_regions_count++] = r;
}

STATIC int less_than_cmp_mem_region(const void *a, const void *b)
{
   const memory_region_t *m1 = a;
   const memory_region_t *m2 = b;

   if (m1->addr < m2->addr)
      return -1;

   if (m1->addr == m2->addr)
      return 0;

   return 1;
}

STATIC void sort_mem_regions(void)
{
   insertion_sort_generic(mem_regions,
                          sizeof(memory_region_t),
                          (u32)mem_regions_count,
                          less_than_cmp_mem_region);
}

void system_mmap_add_ramdisk(uptr start_paddr, uptr end_paddr)
{
   append_mem_region((memory_region_t) {
      .addr = start_paddr,
      .len = end_paddr - start_paddr,
      .type = MULTIBOOT_MEMORY_RESERVED,
      .extra = MEM_REG_EXTRA_RAMDISK
   });

   sort_mem_regions();
}

void *system_mmap_get_ramdisk_vaddr(int ramdisk_index)
{
   int rd_count = 0;

   for (int i = 0; i < mem_regions_count; i++) {

      memory_region_t *m = mem_regions + i;

      if (m->extra & MEM_REG_EXTRA_RAMDISK) {
         if (rd_count == ramdisk_index)
            return KERNEL_PA_TO_VA((uptr)m->addr);

         rd_count++;
      }
   }

   return NULL;
}

STATIC void remove_mem_region(int i)
{
   memory_region_t *ma = mem_regions + i;
   const int rem = mem_regions_count - i - 1;

   memcpy(ma, ma + 1, (size_t) rem * sizeof(memory_region_t));
   mem_regions_count--; /* decrease the number of memory regions */
}

STATIC void swap_mem_regions(int i, int j)
{
   ASSERT(0 <= i && i < mem_regions_count);
   ASSERT(0 <= j && j < mem_regions_count);

   memory_region_t temp = mem_regions[i];
   mem_regions[i] = mem_regions[j];
   mem_regions[j] = temp;
}

STATIC void remove_mem_region_by_swap_with_last(int i)
{
   ASSERT(0 <= i && i < mem_regions_count);
   swap_mem_regions(i, mem_regions_count - 1);
   mem_regions_count--;
}

STATIC void align_mem_regions_to_page_boundary(void)
{
   for (int i = 0; i < mem_regions_count; i++) {

      memory_region_t *ma = mem_regions + i;

      /*
       * Unfortunately, in general we cannot rely on the memory regions to be
       * page-aligned (while they will be in most of the cases). Therefore,
       * we have to forcibly approximate the regions at page-boundaries.
       */
      const u64 ma_end = round_up_at64(ma->addr + ma->len, PAGE_SIZE);
      ma->addr &= ~((u64)PAGE_SIZE - 1); /* Don't use PAGE_MASK here: it causes
                                            truncation on 32-bit systems */
      ma->len = ma_end - ma->addr;
   }
}

STATIC void merge_adj_mem_regions(void)
{
   for (int i = 0; i < mem_regions_count - 1; i++) {

      memory_region_t *ma = mem_regions + i;
      memory_region_t *ma_next = ma + 1;

      if (ma_next->type != ma->type || ma_next->extra != ma->extra)
         continue;

      if (ma_next->addr != ma->addr + ma->len)
         continue;

      /* If we got here, we hit two adjacent regions having the same type */

      ma->len += ma_next->len;
      remove_mem_region(i + 1);
      i--; /* compensate the i++ in the for loop: we have to keep the index */
   }
}

STATIC bool handle_region_overlap(int r1_index, int r2_index)
{
   if (r1_index == r2_index)
      return false;

   memory_region_t *r1 = mem_regions + r1_index;
   memory_region_t *r2 = mem_regions + r2_index;

   u64 s1 = r1->addr;
   u64 s2 = r2->addr;
   u64 e1 = r1->addr + r1->len;
   u64 e2 = r2->addr + r2->len;

   if (s2 < s1) {
      /*
       * Case 0: region 2 starts before region 1.
       * All the cases below are possible (mirrored).
       *
       *              +----------------------+
       *              |       region 1       |
       *              +----------------------+
       *  +----------------------+
       *  |       region 2       |
       *  +----------------------+
       */

      return handle_region_overlap(r2_index, r1_index);
   }

   if (s2 >= e1) {

      /*
       * Case 1: no-overlap.
       *
       *
       *  +----------------------+
       *  |       region 1       |
       *  +----------------------+
       *                         +----------------------+
       *                         |       region 2       |
       *                         +----------------------+
       *
       * Reason: the regions do not overlap.
       */
      return false;
   }


   if (s1 <= s2 && e2 <= e1) {

      /*
       * Case 2: full overlap (region 2 is inside 1)
       *
       *  +---------------------------------+
       *  |            region 1             |
       *  +---------------------------------+
       *            +---------------+
       *            |   region 2    |
       *            +---------------+
       *
       * Corner case 2a:
       *  +---------------------------------+
       *  |            region 1             |
       *  +---------------------------------+
       *  +---------------------------------+
       *  |            region 2             |
       *  +---------------------------------+
       *
       * Corner case 2b:
       *  +---------------------------------+
       *  |            region 1             |
       *  +---------------------------------+
       *  +---------------+
       *  |   region 2    |
       *  +---------------+
       *
       * Corner case 2c:
       *  +---------------------------------+
       *  |            region 1             |
       *  +---------------------------------+
       *                    +---------------+
       *                    |   region 2    |
       *                    +---------------+
       */

      if (r1->type >= r2->type) {

         /*
          * Region 1's type is stricter than region 2's. Just remove region 2.
          */

         remove_mem_region_by_swap_with_last(r2_index);

      } else {

         /*
          * Region 2's type is stricter, we need to split region 1 in two parts:
          *  +---------------+               +-------------------+
          *  |  region 1-1   |               |     region 1-2    |
          *  +---------------+               +-------------------+
          *                  +---------------+
          *                  |   region 2    |
          *                  +---------------+
          */

         if (s1 == s2 && e1 == e2) {

            /*
             * Corner case 2a: regions 1-1 and 1-2 are empty.
             */

            remove_mem_region_by_swap_with_last(r1_index);

         } else if (s1 == s2) {

            /* Corner case 2b: region 1-1 is empty" */

            r1->addr = e2;
            r1->len = e1 - r1->addr;

         } else if (e1 == e2) {

            /* Corner case 2c: region 1-2 is empty" */

            r1->len = s2 - s1;

         } else {

            /* Base case */

            r1->len = s2 - s1;

            append_mem_region((memory_region_t) {
               .addr = e2,
               .len = (e1 - e2),
               .type = r1->type,
               .extra = r1->extra
            });
         }
      }

      return true;
   }

   if (s1 <= s2 && s2 < e1 && e2 > e1) {

      /*
       * Case 3: partial overlap.
       *
       *  +---------------------------------+
       *  |            region 1             |
       *  +---------------------------------+
       *                    +---------------------------+
       *                    |          region 2         |
       *                    +---------------------------+
       *
       * Corner case 3a:
       *
       *  +----------------------------+
       *  |          region 1          |
       *  +----------------------------+
       *  +--------------------------------------------+
       *  |                  region 2                  |
       *  +--------------------------------------------+
       */

      if (r1->type >= r2->type) {

         /*
          * Region 1's type is stricter than region 2's. Move region 2's start.
          *  +---------------------------------+
          *  |            region 1             |
          *  +---------------------------------+
          *                                    +-----------+
          *                                    |  region 2 |
          *                                    +-----------+
          */

         r2->addr = e1;
         r2->len = e2 - r2->addr;

      } else {

         /*
          * Region 2's type is stricter, move region 1's end.
          *
          *  +-----------------+
          *  |    region 1     |
          *  +-----------------+
          *                    +---------------------------+
          *                    |          region 2         |
          *                    +---------------------------+
          */

         if (s1 == s2) {
            /* Corner case 3a: region 1 would become empty. Remove it. */
            remove_mem_region_by_swap_with_last(r1_index);
         } else {
            /* Base case: just shrink region 1 */
            r1->len = s2 - s1;
         }
      }

      return true;
   }

   /*
    * There should NOT be any unhandled cases.
    */
   NOT_REACHED();
}

STATIC void handle_overlapping_regions(void)
{
   bool any_overlap;

   do {

      any_overlap = false;

      for (int i = 0; i < mem_regions_count - 1; i++)
         if (handle_region_overlap(i, i + 1))
            any_overlap = true;

      sort_mem_regions();

   } while (any_overlap);
}

STATIC void fix_mem_regions(void)
{
   align_mem_regions_to_page_boundary();
   sort_mem_regions();
   merge_adj_mem_regions();
   handle_overlapping_regions();
}

STATIC void add_kernel_phdrs_to_mmap(void)
{
   Elf_Ehdr *h = (Elf_Ehdr*)(KERNEL_PA_TO_VA(KERNEL_PADDR));
   Elf_Phdr *phdrs = (void *)h + h->e_phoff;

   for (int i = 0; i < h->e_phnum; i++) {

      Elf_Phdr *phdr = phdrs + i;

      if (phdr->p_type != PT_LOAD)
         continue;

      append_mem_region((memory_region_t) {
         .addr = phdr->p_paddr,
         .len = phdr->p_memsz,
         .type = MULTIBOOT_MEMORY_RESERVED,
         .extra = MEM_REG_EXTRA_KERNEL
      });
   }
}

STATIC void set_lower_and_upper_kb(void)
{
   __mem_lower_kb = 0;
   __mem_upper_kb = 0;

   for (int i = 0; i < mem_regions_count; i++) {

      memory_region_t *m = mem_regions + i;

      if (m->type == MULTIBOOT_MEMORY_AVAILABLE) {
         __mem_lower_kb = (u32) (m->addr / KB);
         break;
      }
   }

   for (int i = mem_regions_count - 1; i >= 0; i--) {

      memory_region_t *m = mem_regions + i;

      if (m->type == MULTIBOOT_MEMORY_AVAILABLE) {
         __mem_upper_kb = (u32) ((m->addr + m->len) / KB);
         break;
      }
   }
}

void system_mmap_set(multiboot_info_t *mbi)
{
   uptr ma_addr = mbi->mmap_addr;

   /* We want to keep the first 64 KB as reserved */
   append_mem_region((memory_region_t) {
      .addr = 0,
      .len = 64 * KB,
      .type = MULTIBOOT_MEMORY_RESERVED,
      .extra = MEM_REG_EXTRA_LOWMEM
   });

   while (ma_addr < mbi->mmap_addr + mbi->mmap_length) {

      multiboot_memory_map_t *ma = (void *)ma_addr;
      append_mem_region((memory_region_t) {
         .addr = ma->addr,
         .len = ma->len,
         .type = ma->type,
         .extra = 0
      });
      ma_addr += ma->size + 4;
   }

   add_kernel_phdrs_to_mmap();
   fix_mem_regions();
   set_lower_and_upper_kb();
}

int system_mmap_get_region_of(uptr paddr)
{
   for (int i = 0; i < mem_regions_count; i++) {

      memory_region_t *m = mem_regions + i;

      if (m->addr <= paddr && paddr < (m->addr + m->len))
         return i;
   }

   return -1;
}

bool
linear_map_mem_region(memory_region_t *r, uptr *vbegin, uptr *vend)
{
   if (r->addr >= LINEAR_MAPPING_SIZE)
      return false;

   const uptr pbegin = (uptr) r->addr;
   const uptr pend = MIN((uptr)(r->addr + r->len), (uptr)LINEAR_MAPPING_SIZE);
   const bool rw = (r->type == MULTIBOOT_MEMORY_AVAILABLE) ||
                   (r->extra & MEM_REG_EXTRA_KERNEL);

   const size_t page_count = (pend - pbegin) >> PAGE_SHIFT;

   *vbegin = (uptr)KERNEL_PA_TO_VA(pbegin);
   *vend = (uptr)KERNEL_PA_TO_VA(pend);

   size_t count =
      map_pages(get_kernel_pdir(),
                (void *)*vbegin,
                pbegin,
                page_count,
                true, /* big pages allowed */
                false, /* user-accessible */
                rw);

   if (count != page_count)
      panic("kmalloc: unable to map regions in the virtual space");

   if (!get_curr_pdir() && pend >= 4 * MB)
      set_page_directory(get_kernel_pdir());

   return true;
}

static const char *mem_region_extra_to_str(u32 e)
{
   switch (e) {
      case MEM_REG_EXTRA_RAMDISK:
         return "RDSK";
      case MEM_REG_EXTRA_KERNEL:
         return "KRNL";
      case MEM_REG_EXTRA_LOWMEM:
         return "LMRS";
      case MEM_REG_EXTRA_FRAMEBUFFER:
         return "FBUF";
      default:
         return "    ";
   }
}

void dump_memory_map(const char *msg, memory_region_t *regions, int count)
{
   printk(NO_PREFIX "\n");
   printk(NO_PREFIX "%s\n\n", msg);
   printk(NO_PREFIX "           START                 END        (T, Extr)\n");

   for (int i = 0; i < count; i++) {

      memory_region_t *ma = regions + i;

      printk(NO_PREFIX "%02d) 0x%016llx - 0x%016llx (%d, %s) [%8u KB]\n", i,
             ma->addr, ma->addr + ma->len,
             ma->type, mem_region_extra_to_str(ma->extra), ma->len / KB);
   }

   printk(NO_PREFIX "\n");
}

void dump_system_memory_map(void)
{
   dump_memory_map("System's memory map:", mem_regions, mem_regions_count);

#ifdef __arch__x86__
   dump_var_mtrrs();
#endif
}
