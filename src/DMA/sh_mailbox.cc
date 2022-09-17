#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "DMA/sh_mailbox.h"

#define BUS_TO_PHYS(x) ((x) & ~0xC0000000)

#define PAGE_SIZE 4096
#define ALIGN_UP(p, alignment) \
  (((uintptr_t)(p) + (alignment)-1) & ~((alignment)-1))

#define FATAL_ERROR(msg)  \
  {                       \
    fprintf(stderr, msg); \
    exit(-1);             \
  }

// Message IDs for different mailbox GPU memory allocation messages
#define MEM_ALLOC_MESSAGE 0x3000c  // This message is 3 u32s: numBytes, alignment and flags
#define MEM_FREE_MESSAGE 0x3000f   // This message is 1 u32: handle
#define MEM_LOCK_MESSAGE 0x3000d   // 1 u32: handle
#define MEM_UNLOCK_MESSAGE 0x3000e // 1 u32: handle

// Memory allocation flags
#define MEM_ALLOC_FLAG_DIRECT (1 << 2) // Allocate uncached memory that bypasses L1 and L2 cache on loads and stores

template <int numPayloads>
struct mBoxMessage
{
  uint32_t size;
  uint32_t reqCode;
  uint32_t mId;
  uint32_t mBytes;
  uint32_t dataBytes;
  union
  {
    uint32_t payload[numPayloads];
    uint32_t result;
  };
  uint32_t msgSentinel;

  // constructor method.
  mBoxMessage(uint32_t msgId) : size(sizeof(*this)),
                                reqCode(0),
                                mId(msgId),
                                mBytes(sizeof(uint32_t) * numPayloads),
                                dataBytes(sizeof(uint32_t) * numPayloads),
                                msgSentinel(0)
  {
    ;
  }
};

/*
 * mem_fd should open for /dev/mem
 * TODO: need to find a way to fd open for a given program lifetype.
 */
static int32_t mem_fd = -1;

static void send_mailbox_command(void *buf)
{
  int vcio = open("/dev/vcio", 0);
  if (vcio < 0)
    FATAL_ERROR("Failed to open VideoCore kernel mailbox!");
  int ret = ioctl(vcio, _IOWR(/*MAJOR_NUM=*/100, 0, char *), buf);
  close(vcio);
  if (ret < 0)
    FATAL_ERROR("SendMailbox failed in ioctl!");
}

static uint32_t mailbox_message(uint32_t msgId, uint32_t payload0)
{
  mBoxMessage<1> msg(msgId);
  msg.payload[0] = payload0;
  send_mailbox_command(&msg);
  return msg.result;
}

static uint32_t mailbox_message(uint32_t msgId, uint32_t payload0, uint32_t payload1, uint32_t payload2)
{
  mBoxMessage<3> msg(msgId);
  msg.payload[0] = payload0;
  msg.payload[1] = payload1;
  msg.payload[2] = payload2;
  send_mailbox_command(&msg);
  return msg.result;
}

uint32_t sh_init()
{
  if ((mem_fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC)) < 0)
  {
    FATAL_ERROR("Error: can't open /dev/mem, run using sudo\n");
  }
  return mem_fd;
}

uint32_t sh_fini()
{
  return close(mem_fd);
}

uint32_t sh_alloc(const uint32_t len, UncachedMem *mem)
{
  if (mem_fd < 0)
    FATAL_ERROR("Error: can't access /dev/mem, confirm sh_init() is called\n");

  mem->len = ALIGN_UP(len, PAGE_SIZE);
  mem->allocHandle = mailbox_message(MEM_ALLOC_MESSAGE,
                                mem->len,             /* size */
                                PAGE_SIZE,            /* alignment */
                                MEM_ALLOC_FLAG_DIRECT /* flags */
  );
  mem->pAddr = mailbox_message(MEM_LOCK_MESSAGE, mem->allocHandle);
  mem->vAddr = mmap(0, mem->len, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, BUS_TO_PHYS(mem->pAddr));
  if (mem->vAddr == MAP_FAILED)
  {
    FATAL_ERROR("Failed to mmap GPU memory!");
  }
  return mem->len;
}

void sh_dealloc(const UncachedMem *mem)
{
  munmap(mem->vAddr, mem->len);
  mailbox_message(MEM_UNLOCK_MESSAGE, mem->allocHandle);
  mailbox_message(MEM_FREE_MESSAGE, mem->allocHandle);
  ;
}

#ifdef TEST_SH_MAILBOX

int main()
{
  UncachedMem mem;
  sh_init();

  sh_alloc(1024, &mem);
  printf("Size: %d, vmem %p, pmem %p, bmem %p\n", mem.len, mem.vAddr, mem.pAddr, mem.bAddr);
  sh_dealloc(&mem);

  sh_alloc(10024, &mem);
  printf("Size: %d, vmem %p, pmem %p, bmem %p\n", mem.len, mem.vAddr, mem.pAddr, mem.bAddr);
  sh_dealloc(&mem);

  sh_fini();
}

#endif // TEST_SH_MAILBOX