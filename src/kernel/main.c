#include "arch/mod.h"
#include "lib/mod.h"
#include "mem/mod.h"
#include "trap/mod.h"
#include "proc/mod.h"

volatile static int started = 0;

#define TEST_PAGE_MAX 64

extern char trampoline[];

static uint64 test_user_pages[TEST_PAGE_MAX];
static uint64 test_pgtbl_pages[TEST_PAGE_MAX];
static uint64 test_allocated[TEST_PAGE_MAX];
static uint32 test_user_npage;
static uint32 test_pgtbl_npage;

static void test_record_page(uint64 *pages, uint32 *count, uint64 page,
                             const char *duplicate_message)
{
    assert(*count < TEST_PAGE_MAX, "pgtbl test: too many pages.");
    for (uint32 i = 0; i < *count; i++)
        assert(pages[i] != page, duplicate_message);
    pages[(*count)++] = page;
}

static void test_collect_pages(pgtbl_t pgtbl, uint32 level)
{
    test_record_page(test_pgtbl_pages, &test_pgtbl_npage, (uint64)pgtbl,
                     "pgtbl test: shared page-table page.");
    for (uint32 i = 0; i < PGSIZE / sizeof(pte_t); i++)
    {
        pte_t pte = pgtbl[i];
        if (!(pte & PTE_V))
            continue;
        uint64 pa = PTE_TO_PA(pte);
        if (PTE_CHECK(pte)) {
            assert(level > 1, "pgtbl test: invalid non-leaf.");
            test_collect_pages((pgtbl_t)pa, level - 1);
        } else if (pa != (uint64)trampoline) {
            assert(level == 1, "pgtbl test: unexpected huge page.");
            test_record_page(test_user_pages, &test_user_npage, pa,
                             "pgtbl test: shared user page.");
        }
    }
}

static void test_expect_recycled(uint64 *expected, uint32 count, bool in_kernel)
{
    bool matched[TEST_PAGE_MAX];
    memset(matched, 0, sizeof(matched));

    for (uint32 i = 0; i < count; i++)
    {
        uint64 page = (uint64)pmem_alloc(in_kernel);
        bool found = false;
        test_allocated[i] = page;
        for (uint32 j = 0; j < count; j++)
        {
            if (expected[j] == page)
            {
                assert(!matched[j], "pgtbl test: page recycled twice.");
                matched[j] = true;
                found = true;
                break;
            }
        }
        assert(found, "pgtbl test: page was not recycled.");
    }
    for (uint32 i = 0; i < count; i++)
        pmem_free(test_allocated[i]);
}

static void test_map_page(pgtbl_t pgtbl, uint64 va, int perm, uint8 value)
{
    uint64 page = (uint64)pmem_alloc(false);
    memset((void *)page, value, PGSIZE);
    vm_mappages(pgtbl, va, page, PGSIZE, perm | PTE_U);
}

static void test_pgtbl_copy_destroy()
{
    static const uint64 vas[] = {
        USER_BASE, USER_BASE + PGSIZE, USER_BASE + 2 * PGSIZE,
        MMAP_BEGIN, MMAP_BEGIN + PGSIZE, MMAP_BEGIN + 4 * PGSIZE,
        TRAPFRAME - 3 * PGSIZE, TRAPFRAME - 2 * PGSIZE,
        TRAPFRAME - PGSIZE,
    };
    mmap_region_t mmap_2 = {
        .begin = MMAP_BEGIN + 4 * PGSIZE,
        .npages = 1,
        .next = NULL,
    };
    mmap_region_t mmap_1 = {
        .begin = MMAP_BEGIN,
        .npages = 2,
        .next = &mmap_2,
    };
    uint64 old_tf = (uint64)pmem_alloc(false);
    uint64 new_tf = (uint64)pmem_alloc(false);
    pgtbl_t old = proc_pgtbl_init(old_tf);
    pgtbl_t new = proc_pgtbl_init(new_tf);

    test_map_page(old, vas[0], PTE_R | PTE_X, 0x11);
    for (uint32 i = 1; i < sizeof(vas) / sizeof(vas[0]); i++)
        test_map_page(old, vas[i], PTE_R | PTE_W, 0x11 + i);

    uvm_copy_pgtbl(old, new, USER_BASE + 3 * PGSIZE, 3, &mmap_1);
    for (uint32 i = 0; i < sizeof(vas) / sizeof(vas[0]); i++)
    {
        pte_t *old_pte = vm_getpte(old, vas[i], false);
        pte_t *new_pte = vm_getpte(new, vas[i], false);
        assert(old_pte != NULL && new_pte != NULL,
               "pgtbl test: copied PTE missing.");
        assert((*old_pte & PTE_V) && (*new_pte & PTE_V),
               "pgtbl test: copied PTE invalid.");
        assert(PTE_FLAGS(*old_pte) == PTE_FLAGS(*new_pte),
               "pgtbl test: permissions changed.");
        uint64 old_pa = PTE_TO_PA(*old_pte);
        uint64 new_pa = PTE_TO_PA(*new_pte);
        assert(old_pa != new_pa, "pgtbl test: user page shared.");
        for (uint32 j = 0; j < PGSIZE; j++)
            assert(((uint8 *)old_pa)[j] == ((uint8 *)new_pa)[j],
                   "pgtbl test: page content changed.");
    }

    pte_t *old_first = vm_getpte(old, USER_BASE, false);
    pte_t *new_first = vm_getpte(new, USER_BASE, false);
    ((uint8 *)PTE_TO_PA(*new_first))[0] = 0xff;
    assert(((uint8 *)PTE_TO_PA(*old_first))[0] == 0x11,
           "pgtbl test: copied page is not isolated.");

    test_user_npage = 0;
    test_pgtbl_npage = 0;
    test_collect_pages(old, 3);
    test_collect_pages(new, 3);
    assert(test_user_npage == 20, "pgtbl test: unexpected user-page count.");

    uvm_destroy_pgtbl(old);
    uvm_destroy_pgtbl(new);
    test_expect_recycled(test_user_pages, test_user_npage, false);
    test_expect_recycled(test_pgtbl_pages, test_pgtbl_npage, true);
    printf("pgtbl copy: content permissions isolation pass\n");
    printf("pgtbl destroy: 20 user pages and %d table pages recycled\n",
           test_pgtbl_npage);
}

int main()
{
    int cpuid = r_tp();

    if (cpuid == 0) {

        print_init();
        printf("cpu %d is booting!\n", cpuid);

        pmem_init();
        mmap_init();
        test_pgtbl_copy_destroy();
        kvm_init();
        kvm_inithart();
        trap_kernel_init();
        trap_kernel_inithart();
        proc_make_first();
        __sync_synchronize();
        started = 1;
    } else {

        while (started == 0)
            ;
        __sync_synchronize();
        printf("cpu %d is booting!\n", cpuid);
        kvm_inithart();
        trap_kernel_inithart();
    }
    while (1)
        ;
}
