#include "mod.h"

// mmap_region_node_t 仓库、不可分配的链表头节点及仓库自旋锁。
static mmap_region_node_t node_list[N_MMAP];
static mmap_region_node_t list_head;
static spinlock_t list_lk;

void mmap_init()
{
    // TODO: 初始化节点仓库、空闲链表和自旋锁。
}

mmap_region_t *mmap_region_alloc()
{
    // TODO: 加锁并从空闲链表中分配一个 mmap_region。
    return NULL;
}

void mmap_region_free(mmap_region_t *mmap)
{
    // TODO: 清理节点状态并加锁归还到空闲链表。
}

// 输出当前可用的 mmap_region_node_t 链，仅用于调试。
void mmap_show_nodelist()
{
    spinlock_acquire(&list_lk);

    mmap_region_node_t *tmp = list_head.next;
    int node = 0, index = 0;
    while (tmp)
    {
        index = tmp - &(node_list[0]);
        printf("node %d index = %d\n", node++, index);
        tmp = tmp->next;
    }

    spinlock_release(&list_lk);
}
