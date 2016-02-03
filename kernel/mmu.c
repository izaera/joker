#include <stdio.h>

#include "kernel.h"
#include "mmu.h"

#define _ADD(x, y) (((uint32_t)x)+((uint32_t)y))
#define _SUB(x, y) (((uint32_t)x)-((uint32_t)y))
#define _LT(x, y)  (((uint32_t)x)<((uint32_t)y))
#define _LTE(x, y) (((uint32_t)x)<=((uint32_t)y))
#define _GT(x, y)  (((uint32_t)x)>((uint32_t)y))
#define _GTE(x, y) (((uint32_t)x)>=((uint32_t)y))

/* Ranges *********************************************************************/
typedef struct _range_t {
  uint32_t start;
  uint32_t end;
} range_t;

static int range_overlaps(range_t* range, range_t* other) {
  return range->start < other->start && range->end >= other->start;
}

static int range_contains(range_t* range, range_t* other) {
  return range->start <= other->start && range->end >= other->end;
}

static void ranges_append(range_t* ranges, size_t *count, range_t* range) {
  // TODO: sort and coalesce blocks
  ranges[(*count)++] = *range;
}

static void ranges_insert(range_t* ranges, size_t *count, size_t index,
    range_t* range) {
  for (size_t i = *count; i > index; i--) {
    ranges[i] = ranges[i - 1];
  }
  ranges[index] = *range;
  (*count)++;
}

static void ranges_remove(range_t* ranges, size_t *count, size_t index) {
  for (size_t i = index + 1; i < *count; i++) {
    ranges[i - 1] = ranges[i];
  }
  (*count)--;
}

static void ranges_lock(range_t* ranges, size_t *count, range_t* lock) {
  for (size_t i = 0; i < *count; i++) {
    range_t* range = ranges + i;

    if (range_contains(lock, range)) {
      ranges_remove(ranges, count, i);
      i--;
    } else if (range_overlaps(range, lock)) {
      range->end = lock->start - 1;
    } else if (range_overlaps(lock, range)) {
      range->start = lock->end + 1;
    } else if (range_contains(range, lock)) {
      if (range->start == lock->start) {
        range->start = lock->end + 1;
      } else if (range->end == lock->end) {
        range->end = lock->start - 1;
      } else {
        ranges_insert(ranges, count, i, range);
        range[i].end = lock->start - 1;
        ranges[i + 1].start = lock->end + 1;
        i++;
      }
    }
  }
}

static void ranges_dump(range_t* ranges, size_t count) {
  for (size_t i = 0; i < count; i++) {
    printf("%X-%X ", ranges[i].start, ranges[i].end);
  }
  printf("\n");
}

/* Nodes **********************************************************************/
#define _NODE_MAGIC    0xDED0
#define _NODE_MIN_SIZE 1024   /* should be at least sizeof(_node) */

typedef struct _node_t {
  uint16_t magic;
  uint16_t free;
  void* end;
  struct _node_t* prev;
  struct _node_t* next;
  uint8_t data[];
} node_t;

static void* last_valid_address = 0;
static node_t* first_node = 0;
static node_t* last_node = 0;

static size_t node_size(node_t* node) {
  return _SUB(node->end, node->data) + 1;
}

static size_t node_data_size(node_t* node) {
  return node_size(node) - sizeof(node_t);
}

static int node_is_valid(node_t* node) {
  if (node->magic != _NODE_MAGIC || node->next->magic != _NODE_MAGIC) {
    return 0;
  }

  return 1;
}

static node_t* node_for_data(void* data) {
  node_t* node = (node_t*) _SUB(data, sizeof(node_t));
  k_ensure(node_is_valid(node));
  return node;
}

static int node_is_adjacent_left(node_t* node, node_t* other) {
  return _ADD(node->end, 1) == (uint32_t) other;
}

static void nodes_append(void* start, void* end) {
  node_t* new_node = (node_t*) start;

  new_node->magic = _NODE_MAGIC;
  new_node->free = 1;
  new_node->end = end;
  new_node->next = 0;

  if (!first_node) {
    first_node = new_node;
    new_node->prev = 0;
  } else {
    last_node->next = new_node;
    new_node->prev = last_node;
  }

  last_node = new_node;
}

static void nodes_split(node_t* node, size_t size) {
  node_t* next = node->next;
  void* end = node->end;

  node->end = (void*) _ADD(node, sizeof(node_t)+size-1);
  node->next = (node_t*) _ADD(node->end, 1);
  next->prev = node->next;

  node_t* new_node = node->next;

  new_node->magic = _NODE_MAGIC;
  new_node->free = 1;
  new_node->end = end;
  new_node->prev = node;
  new_node->next = next;
}

static void nodes_coalesce(node_t* node) {
  k_ensure(node->free);

  while (node) {
    node_t* next = node->next;

    if (next && next->free && node_is_adjacent_left(node, next)) {
      next->magic = 0;

      node->next = next->next;
      node->end = next->end;

      if (node->next) {
        node->next->prev = node;
      } else {
        last_node = node;
      }
    }

    node_t* prev = node->prev;

    if (prev && prev->free) {
      node = prev;
    } else {
      node = 0;
    }
  }
}

/* MMU ************************************************************************/
void mmu_init(mmap_entry_t* mmap_addr, uint32_t mmap_length, void* kernel_start,
    void* kernel_end, void* stack_start, void* stack_end) {

  printf("mmu: loading memory map\n");

  // We assume this memory is always free because kernel is loaded in hi memory
  range_t* ranges = (range_t*) MMU_MIN_ADDRESS;
  size_t rcount = 0;

  // Visit mmap_entries
  for (mmap_entry_t* mma = mmap_addr; _SUB(mma, mmap_addr) < mmap_length; mma =
      (mmap_entry_t*) _ADD(mma, mma->entry_size + 4)) {

    uint64_t base_addr = mma->base_addr;
    uint64_t end_addr = mma->base_addr + mma->length - 1;

    // Log found block
    printf("mmu: found %s block %llX-%llX\n",
        (mma->type == MMAP_TYPE_AVAILABLE) ? "usable" : "locked", base_addr,
        end_addr);

    // Gather free blocks
    if ((mma->type == MMAP_TYPE_AVAILABLE) && (base_addr <= MMU_MAX_ADDRESS)) {

      // Calculate last valid address
      if (_GT(end_addr, last_valid_address)) {
        last_valid_address = (void*) (uint32_t) end_addr;
      }

      // Impose low limit
      if (base_addr < MMU_MIN_ADDRESS) {
        base_addr = MMU_MIN_ADDRESS;
      }

      // Impose high limit
      if (end_addr > MMU_MAX_ADDRESS) {
        end_addr = MMU_MAX_ADDRESS;
      }

      // Register range
      range_t range = { base_addr, end_addr };
      ranges_append(ranges, &rcount, &range);
    }
  }

  // Lock kernel in memory
  range_t kernel_range = { (uint32_t) kernel_start, (uint32_t) kernel_end };
  ranges_lock(ranges, &rcount, &kernel_range);

  // Lock stack in memory
  range_t stack_range = { (uint32_t) stack_start, (uint32_t) stack_end };
  ranges_lock(ranges, &rcount, &stack_range);

  // Copy nodes to stack to avoid overwriting them
  range_t ranges_copy[rcount];
  for (size_t i = 0; i < rcount; i++) {
    ranges_copy[i] = ranges[i];
  }

  // Register nodes
  for (size_t i = 0; i < rcount; i++) {
    if ((ranges_copy[i].end - ranges_copy[i].start) > sizeof(node_t)) {
      nodes_append((void*) ranges_copy[i].start, (void*) ranges_copy[i].end);
    }
  }

  printf("mmu: last valid address is %X\n", mmu_last_address());
}

void* mmu_alloc(size_t size) {
  for (node_t* node = first_node; node; node = node->next) {
    if (node->free) {
      size_t s = node_size(node);

      if (s >= size) {

        if ((s - size) >= _NODE_MIN_SIZE) {
          nodes_split(node, size);
        }

        node->free = 0;
        return node->data;
      }
    }
  }

  return (void*) 0;
}

void* mmu_realloc(void* ptr, size_t size) {
  uint8_t* new_data = mmu_alloc(size);

  node_t* node = node_for_data(ptr);
  size_t data_size = node_data_size(node);
  if (data_size > size) {
    data_size = size;
  }

  for (size_t i = 0; i < data_size; i++) {
    new_data[i] = node->data[i];
  }

  mmu_free(ptr);

  return new_data;
}

void mmu_free(void* ptr) {
  node_t* node = node_for_data(ptr);

  k_ensure(!node->free);

  node->free = 1;

  nodes_coalesce(node);
}

void* mmu_last_address() {
  return last_valid_address;
}

void mmu_dump() {
  printf("mmu_dump: ");
  for (node_t* node = first_node; node; node = node->next) {
    if (node != first_node) {
      printf(", ");
    }
    printf("%c(%X-%X)", node->free ? 'o' : 'x', node, node->end);
  }
  printf("\n");
}
