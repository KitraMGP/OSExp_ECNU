#include "mod.h"

// mmap_region_node_t 仓库、不可分配的链表头节点及仓库自旋锁。
static mmap_region_node_t node_list[N_MMAP];
static mmap_region_node_t list_head;
static spinlock_t list_lk;

// 初始化空闲链表：按 node_list[0] 到 node_list[N_MMAP - 1] 的顺序串联全部节点，
// 再令哨兵头节点 list_head 指向首个空闲节点。
// 结果：list_head -> node[0] -> node[1] -> ... -> node[N_MMAP - 1] -> NULL。
void mmap_init()
{
    spinlock_init(&list_lk, "mmap_region");
    memset(&list_head, 0, sizeof(list_head));

    for (uint32 i = 0; i < N_MMAP; i++)
    {
        memset(&node_list[i], 0, sizeof(node_list[i]));
        node_list[i].next = i + 1 < N_MMAP ? &node_list[i + 1] : NULL;
    }
    list_head.next = &node_list[0];
}

// 从空闲链表表头分配一个节点，即执行“摘除首元节点”。
// 操作前：list_head -> node -> next；操作后：list_head -> next，同时 node 与空闲链断开。
mmap_region_t *mmap_region_alloc()
{
    mmap_region_node_t *node;

    spinlock_acquire(&list_lk);
    // 保存首元节点，并让哨兵头节点跳过它，指向原链表中的第二个节点。
    node = list_head.next;
    assert(node != NULL, "mmap_region_alloc: no free node.");
    list_head.next = node->next;
    // 清空被摘节点的仓库 next，避免它继续指向空闲链表。
    node->next = NULL;
    memset(&node->mmap, 0, sizeof(node->mmap));
    spinlock_release(&list_lk);

    return &node->mmap;
}

// 将节点归还空闲链表，采用 O(1) 的“头插法”。
// 操作前：list_head -> first；操作后：list_head -> node -> first。
void mmap_region_free(mmap_region_t *mmap)
{
    uint64 addr = (uint64)mmap;
    uint64 first = (uint64)&node_list[0];
    uint64 end = (uint64)&node_list[N_MMAP];

    assert(addr >= first && addr < end, "mmap_region_free: invalid node.");
    assert((addr - first) % sizeof(mmap_region_node_t) == 0,
           "mmap_region_free: unaligned node.");

    mmap_region_node_t *node = (mmap_region_node_t *)mmap;
    spinlock_acquire(&list_lk);
    // 只读遍历空闲链表，确认待归还节点尚不在链中，防止重复插入形成环或重复节点。
    for (mmap_region_node_t *tmp = list_head.next; tmp != NULL; tmp = tmp->next)
        assert(tmp != node, "mmap_region_free: double free.");

    // 先清理节点保存的 mmap 区域信息，再把它插到原首元节点之前。
    memset(&node->mmap, 0, sizeof(node->mmap));
    node->next = list_head.next;
    list_head.next = node;
    spinlock_release(&list_lk);
}

// 从哨兵头节点之后开始只读遍历空闲链表，依次输出节点在 node_list 中的下标；
// 不摘除、不插入节点，也不改变任何 next 指针，仅用于调试。
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
