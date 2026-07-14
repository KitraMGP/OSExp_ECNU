#include "mod.h"

// 内核空间和用户空间的可分配物理页分开描述
static alloc_region_t kern_region, user_region;

// 物理内存的初始化
// 本质上就是填写kern_region和user_region, 包括基本数值和空闲链表
void pmem_init(void)
{
    // 内核可分配区域：可分配 KERN_PAGES 个页面
    spinlock_init(&kern_region.lk, "kern_region");
    kern_region.begin = (uint64)ALLOC_BEGIN;
    kern_region.end = (uint64)ALLOC_BEGIN + KERN_PAGES * PGSIZE;
    kern_region.allocable = KERN_PAGES;
    // 构建链表。本链表利用每个空闲页面的前 64 位存储 next。
    kern_region.list_head.next = (page_node_t *)kern_region.begin;
    for (uint64 pg = kern_region.begin; pg < kern_region.end; pg += PGSIZE)
    {
        page_node_t* node = (page_node_t*)pg;
        node->next = (pg + PGSIZE < kern_region.end) ? (page_node_t*)(pg + PGSIZE) : NULL;
    }

    // 用户可分配区域：可分配剩下所有页面
    spinlock_init(&user_region.lk, "user_region");
    user_region.begin = kern_region.end;
    user_region.end = (uint64)ALLOC_END;
    user_region.allocable = (user_region.end - user_region.begin) / PGSIZE;
    // 构建链表。和上面相同
    user_region.list_head.next = (page_node_t*)user_region.begin;
    for (uint64 pg = user_region.begin; pg < user_region.end; pg += PGSIZE)
    {
        page_node_t* node = (page_node_t*)pg;
        node->next = (pg + PGSIZE < user_region.end) ? (page_node_t*)(pg + PGSIZE) : NULL;
    }
}

// 尝试返回一个可分配的清零后的物理页
// 失败则panic锁死
void* pmem_alloc(bool in_kernel)
{
    page_node_t *page = NULL;
    if (in_kernel)
    {
        spinlock_acquire(&kern_region.lk);
        if (kern_region.list_head.next == NULL || kern_region.allocable == 0)
        {
            panic("pmem_alloc() failed: no available physical pages in kernel region.");
        } else {
            page = kern_region.list_head.next;
            kern_region.list_head.next = kern_region.list_head.next->next;
            kern_region.allocable--;
        }
        spinlock_release(&kern_region.lk);
    } else {
        spinlock_acquire(&user_region.lk);
        if (user_region.list_head.next == NULL || user_region.allocable == 0)
        {
            panic("pmem_alloc() failed: no available physical pages in user region.");
        } else {
            page = user_region.list_head.next;
            user_region.list_head.next = user_region.list_head.next->next;
            user_region.allocable--;
        }
        spinlock_release(&user_region.lk);
    }

    // 清零页面
    memset(page, 0, PGSIZE);

    return page;
}

// 释放一个物理页
// 失败则panic锁死
void pmem_free(uint64 page)
{
    if (page == 0)
        panic("pmem_free(): attempt to free page NULL.");
    if (page % PGSIZE != 0)
        panic("pmem_free(): page is not page-aligned.");
    bool in_kernel = false;
    if (page >= kern_region.begin && page < kern_region.end)
    {
        in_kernel = true;
    } else if (page >= user_region.begin && page < user_region.end) {
        in_kernel = false;
    } else {
        panic("pmem_free(): illegal page.");
    }
    if (in_kernel)
    {
        spinlock_acquire(&kern_region.lk);
        // 将页面插入链表头部
        page_node_t* page_ptr = (page_node_t*)page;
        page_ptr->next = kern_region.list_head.next;
        kern_region.list_head.next = page_ptr;
        kern_region.allocable++;
        spinlock_release(&kern_region.lk);
    } else {
        spinlock_acquire(&user_region.lk);
        // 将页面插入链表头部
        page_node_t* page_ptr = (page_node_t*)page;
        page_ptr->next = user_region.list_head.next;
        user_region.list_head.next = page_ptr;
        user_region.allocable++;
        spinlock_release(&user_region.lk);
    }
}
