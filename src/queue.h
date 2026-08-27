#pragma once

namespace uee {

// Circular doubly-linked list. Insertion is at the next end and removal is at
// the prev end, so the pair behaves as a queue.
struct Link {
    Link* next = nullptr;
    Link* prev = nullptr;
};

inline void initQueue(Link& head) {
    head.next = &head;
    head.prev = &head;
}

// KERNEL: the empty test in QueueRemoveTail @ 0x80005af8
inline bool isEmpty(const Link& head) {
    return head.next == &head && head.prev == &head;
}

// KERNEL: QueueInsertHead @ 0x80005ba8
inline void insertHead(Link& head, Link& node) {
    node.prev = &head;
    node.next = head.next;
    head.next->prev = &node;
    head.next = &node;
}

// KERNEL: QueueUnlink @ 0x80005b88
inline void unlink(Link& node) {
    node.prev->next = node.next;
    node.next->prev = node.prev;
}

// KERNEL: QueueRemoveTail @ 0x80005af8
inline Link* removeTail(Link& head) {
    if (isEmpty(head)) {
        return nullptr;
    }
    Link* node = head.prev;
    unlink(*node);
    return node;
}

// KERNEL: QueueRotate @ 0x80005b38
inline Link* rotate(Link& head) {
    Link* node = removeTail(head);
    if (node == nullptr) {
        return nullptr;
    }
    insertHead(head, *node);
    return head.prev;
}

}  // namespace uee
