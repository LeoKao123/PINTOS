#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/fd.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/syscall.h"


static thread_func start_process NO_RETURN;
static bool load(const char* file_name, void (**eip)(void), void** esp);

/* Initializes user programs in the system by ensuring the main
   thread has a minimal PCB so that it can execute and wait for
   the first user process. Any additions to the PCB should be also
   initialized here if main needs those members */
void userprog_init(void) {
  struct thread* t = thread_current();
  bool success;

  /* Allocate process control block
     It is imoprtant that this is a call to calloc and not malloc,
     so that t->pcb->pagedir is guaranteed to be NULL (the kernel's
     page directory) when t->pcb is assigned, because a timer interrupt
     can come at any time and activate our pagedir */
  // t->cwd = dir_open_root();
  t->pcb = calloc(sizeof(struct process), 1);
  t->cwd = dir_open_root();
  success = t->pcb != NULL;

  /* Kill the kernel if we did not succeed */
  ASSERT(success);
}

/*struct so thread_create can take in multiple arguments */
struct arg_struct {
  char* fn_copy;
  struct wait_data* shared;
  struct dir* cwd;
};

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   process id, or TID_ERROR if the thread cannot be created. */
pid_t process_execute(const char* file_name) {
  char* fn_copy;
  tid_t tid;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page(0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy(fn_copy, file_name, PGSIZE);

  /*Create wait data for future child made in thread_create call*/
  struct wait_data* shared = (struct wait_data*)malloc(sizeof(struct wait_data));
  if (shared == NULL) {
    printf("malloc failed in process execute");
    return TID_ERROR;
  }
  /*Initialize semaphore so parent can use to wait for child*/
  sema_init(&shared->wait, 0);
  /*Initialize the list_elem structure in wait_data*/
  shared->elem.prev = NULL;
  shared->elem.next = NULL;

  /*Add child wait data to parent's list*/
  list_push_back(&thread_current()->child_wait_data, &shared->elem);

  /*Create arguments struct to pass shared data to child thread*/
  struct arg_struct* args = (struct arg_struct*)malloc(sizeof(struct arg_struct));
  if (args == NULL) {
    printf("malloc failed in process execute");
    list_remove(&shared->elem);
    free(shared);
    return TID_ERROR;
  }
  args->fn_copy = fn_copy;
  args->shared = shared;
  args->cwd = thread_current()->cwd;

  /*Grab program name*/
  char* file_name_copy = (char*)calloc(1 + strlen(file_name), sizeof(char));
  memcpy(file_name_copy, file_name, sizeof(char) * strlen(file_name));
  char* rest = file_name_copy;
  char* program_name = strtok_r(file_name_copy, " ", &rest);
  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create(program_name, PRI_DEFAULT, start_process, (void*)args);
  
  if (tid == TID_ERROR) {
    palloc_free_page(fn_copy);
  }
  /*Sema down here because it will delay the exec return until executable laods*/
  sema_down(&shared->wait);
  /*if the program doesn't load, make sure to free shared data*/
  if (shared->load_success == false) {
    list_remove(&shared->elem);
    free(shared);
    free(file_name_copy);
    return TID_ERROR;
  }
  free(file_name_copy);
  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void start_process(void* args) {
  struct arg_struct* arguments = (struct arg_struct*)args;
  char* file_name = arguments->fn_copy;

  struct thread* t = thread_current();
  struct intr_frame if_;
  bool success, pcb_success;

  /* Allocate process control block */
  struct process* new_pcb = malloc(sizeof(struct process));
  success = pcb_success = new_pcb != NULL;

  /*allocate and initialize wait data for this thread and its parent */
  /*Assign shared wait data to child's field*/
  struct wait_data* shared = arguments->shared;
  t->shared_wait_data = shared;
  if (shared != NULL) {
    /*Assign TID to shared data*/
    shared->child_pid = t->tid;
    /*Initialize rest of shared data*/
    shared->child_dead = false;
    shared->exit_code = 0;
    lock_init(&shared->value_lock);
    shared->references = 2;
    sema_init(&shared->parent_lock, 1);
  }

  /* Initialize process control block */
  if (success) {
    // Ensure that timer_interrupt() -> schedule() -> process_activate()
    // does not try to activate our uninitialized pagedir
    new_pcb->pagedir = NULL;
    t->pcb = new_pcb;
    memset(t->pcb->open_files, 0, sizeof(FILE*) * MAX_FS_OPEN);
    t->pcb->open_files_cnt = 3;
    t->pcb->next_open_fd = 3;
    lock_init(&t->pcb->fd_lock);
    t->pcb->open_files[0] = (FILE*) -1;
    t->pcb->open_files[1] = (FILE*) -1;
    t->pcb->open_files[2] = (FILE*) -1;
    t->cwd = dir_reopen(arguments->cwd);

    // Continue initializing the PCB as normal
    t->pcb->main_thread = t;
    strlcpy(t->pcb->process_name, t->name, sizeof t->name);
  }

  /* Initialize interrupt frame and load executable. */
  if (success) {
    memset(&if_, 0, sizeof if_);

    /* We will just save parent process's fpu into the stack
       of this new process. Once saved, we will clear out the
       register. */
    asm volatile("fnsave (%0); fninit" : : "g"(&if_.fpu_reg));
    if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
    if_.cs = SEL_UCSEG;
    if_.eflags = FLAG_IF | FLAG_MBS;
    success = load(file_name, &if_.eip, &if_.esp);
    /*Alerts shared data of load success and ups semaphore afterwards.*/
    t->shared_wait_data->load_success = success;
  }
  sema_up(&t->shared_wait_data->wait);

  /* Handle failure with succesful PCB malloc. Must free the PCB */
  if (!success && pcb_success) {
    // Avoid race where PCB is freed before t->pcb is set to NULL
    // If this happens, then an unfortuantely timed timer interrupt
    // can try to activate the pagedir, but it is now freed memory
    struct process* pcb_to_free = t->pcb;
    t->pcb = NULL;
    free(pcb_to_free);

  }

  /* Clean up. Exit on failure or jump to userspace */
  palloc_free_page(file_name);
  free(arguments);
  if (!success) {
    thread_exit();
  }

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&if_) : "memory");
  NOT_REACHED();
}

/* Waits for process with PID child_pid to die and returns its exit status.
   If it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If child_pid is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given PID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int process_wait(pid_t child_pid UNUSED) {
  struct list_elem* e;
  struct wait_data* elem;
  bool found = false;
  /*Find child data in the parent's list of child data*/
  for (e = list_begin(&thread_current()->child_wait_data);
       e != list_end(&thread_current()->child_wait_data); e = list_next(e)) {
    elem = list_entry(e, struct wait_data, elem);
    if ((pid_t)elem->child_pid == child_pid) {
      found = true;
      break;
    }
  }
  if (!found) {
    return -1;
  }

  if (sema_try_down(&elem->parent_lock)) {
    /*you acquired semaphore so sema_down*/
    sema_down(&elem->parent_lock);
  } else {
    /*there is someone waiting so return -1*/
    return -1;
  }

  if (elem->child_dead) {
    return elem->exit_code;
  }

  return elem->exit_code;
}

/* Free the current process's resources. */
void process_exit(int code) {
  struct thread* cur = thread_current();
  uint32_t* pd;

  if (thread_current()->shared_wait_data != NULL) {
    //acuire lock
    lock_acquire(&thread_current()->shared_wait_data->value_lock);
    //update shared_wait_data
    thread_current()->shared_wait_data->exit_code = code;
    thread_current()->shared_wait_data->child_dead = true;
    thread_current()->shared_wait_data->references -= 1;
    //release lock
    lock_release(&thread_current()->shared_wait_data->value_lock);
  }

  struct list_elem* e;
  struct wait_data* elem;
  bool remove = false;
  struct wait_data* to_remove;
  for (e = list_begin(&thread_current()->child_wait_data);
       e != list_end(&thread_current()->child_wait_data); e = list_next(e)) {
    if (remove) {
      free(elem);
      remove = false;
    }
    elem = list_entry(e, struct wait_data, elem);
    //decrement references
    lock_acquire(&elem->value_lock);
    elem->references -= 1;
    lock_release(&elem->value_lock);
    //if references is less than 0 remove from parent list and free shared_data
    if (elem->references <= 0) {
      list_remove(&elem->elem);
      to_remove = elem;
      remove = true;
    }
  }
  //This is a sentinal to make sure that it removes
  if (remove) {
    free(elem);
    remove = false;
  }

  if (thread_current()->shared_wait_data != NULL) {
    //stop parent thread from waiting
    sema_up(&thread_current()->shared_wait_data->parent_lock);
    //if references is less than 0 remove from parent list and free shared_data
    if (thread_current()->shared_wait_data->references <= 0) {
      list_remove(&thread_current()->shared_wait_data->elem);
      free(thread_current()->shared_wait_data);
    }
  }
  printf("%s: exit(%d)\n", thread_current()->pcb->process_name, code);
  /* If this thread does not have a PCB, don't worry */
  if (cur->pcb == NULL) {
    thread_exit();
    NOT_REACHED();
  }

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pcb->pagedir;
  if (pd != NULL) {
    /* Correct ordering here is crucial.  We must set
         cur->pcb->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
    cur->pcb->pagedir = NULL;
    pagedir_activate(NULL);
    pagedir_destroy(pd);
  }

  /* Free the PCB of this process and kill this thread
     Avoid race where PCB is freed before t->pcb is set to NULL
     If this happens, then an unfortuantely timed timer interrupt
     can try to activate the pagedir, but it is now freed memory */
  struct process* pcb_to_free = cur->pcb;
  cur->pcb = NULL;
  dir_close(cur->cwd);
  for (int i = 3; i < MAX_FS_OPEN; i++) {
    if (pcb_to_free->open_files[i] != NULL) {
      if (pcb_to_free->open_files[i]->dir != NULL) dir_close(pcb_to_free->open_files[i]->dir);
      else file_close(pcb_to_free->open_files[i]->file);
      free(pcb_to_free->open_files[i]);
    }
  }
  file_allow_write(pcb_to_free->loaded_file);
  file_close(pcb_to_free->loaded_file);
  free(pcb_to_free);

  thread_exit();
}

/* Sets up the CPU for running user code in the current
   thread. This function is called on every context switch. */
void process_activate(void) {
  struct thread* t = thread_current();

  /* Activate thread's page tables. */
  if (t->pcb != NULL && t->pcb->pagedir != NULL)
    pagedir_activate(t->pcb->pagedir);
  else
    pagedir_activate(NULL);

  /* Set thread's kernel stack for use in processing interrupts.
     This does nothing if this is not a user process. */
  tss_update();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32 /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32 /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32 /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16 /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr {
  unsigned char e_ident[16];
  Elf32_Half e_type;
  Elf32_Half e_machine;
  Elf32_Word e_version;
  Elf32_Addr e_entry;
  Elf32_Off e_phoff;
  Elf32_Off e_shoff;
  Elf32_Word e_flags;
  Elf32_Half e_ehsize;
  Elf32_Half e_phentsize;
  Elf32_Half e_phnum;
  Elf32_Half e_shentsize;
  Elf32_Half e_shnum;
  Elf32_Half e_shstrndx;
};

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr {
  Elf32_Word p_type;
  Elf32_Off p_offset;
  Elf32_Addr p_vaddr;
  Elf32_Addr p_paddr;
  Elf32_Word p_filesz;
  Elf32_Word p_memsz;
  Elf32_Word p_flags;
  Elf32_Word p_align;
};

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL 0           /* Ignore. */
#define PT_LOAD 1           /* Loadable segment. */
#define PT_DYNAMIC 2        /* Dynamic linking info. */
#define PT_INTERP 3         /* Name of dynamic loader. */
#define PT_NOTE 4           /* Auxiliary info. */
#define PT_SHLIB 5          /* Reserved. */
#define PT_PHDR 6           /* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

static bool setup_stack(void** esp, const char* file_name);
static bool validate_segment(const struct Elf32_Phdr*, struct file*);
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool load(const char* file_name, void (**eip)(void), void** esp) {
  struct thread* t = thread_current();
  struct Elf32_Ehdr ehdr;
  struct file* file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;
  /* Allocate and activate page directory. */
  t->pcb->pagedir = pagedir_create();
  if (t->pcb->pagedir == NULL)
    goto done;
  process_activate();

  char* file_name_copy = (char*) calloc(1 + strlen(file_name), sizeof(char));
  if (file_name_copy == NULL) goto done;
  memcpy(file_name_copy, file_name, sizeof(char) * strlen(file_name));
  char* rest = file_name_copy;
  char* program_name = strtok_r(file_name_copy, " ", &rest);
  /* Open executable file. */
  
  t->pcb->loaded_file = filesys_open(program_name);
  file = t->pcb->loaded_file;

  if (file == NULL) {
    printf("load: %s: open failed\n", program_name);
    goto done;
  }

  file_deny_write(file);

  /* Read and verify executable header. */
  if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr ||
      memcmp(ehdr.e_ident, "\177ELF\1\1\1", 7) || ehdr.e_type != 2 || ehdr.e_machine != 3 ||
      ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Elf32_Phdr) || ehdr.e_phnum > 1024) {
    printf("load: %s: error loading executable\n", program_name);
    goto done;
  }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) {
    struct Elf32_Phdr phdr;

    if (file_ofs < 0 || file_ofs > file_length(file))
      goto done;
    file_seek(file, file_ofs);

    if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
      goto done;
    file_ofs += sizeof phdr;
    switch (phdr.p_type) {
      case PT_NULL:
      case PT_NOTE:
      case PT_PHDR:
      case PT_STACK:
      default:
        /* Ignore this segment. */
        break;
      case PT_DYNAMIC:
      case PT_INTERP:
      case PT_SHLIB:
        goto done;
      case PT_LOAD:
        if (validate_segment(&phdr, file)) {
          bool writable = (phdr.p_flags & PF_W) != 0;
          uint32_t file_page = phdr.p_offset & ~PGMASK;
          uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
          uint32_t page_offset = phdr.p_vaddr & PGMASK;
          uint32_t read_bytes, zero_bytes;
          if (phdr.p_filesz > 0) {
            /* Normal segment.
                     Read initial part from disk and zero the rest. */
            read_bytes = page_offset + phdr.p_filesz;
            zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
          } else {
            /* Entirely zero.
                     Don't read anything from disk. */
            read_bytes = 0;
            zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
          }
          if (!load_segment(file, file_page, (void*)mem_page, read_bytes, zero_bytes, writable))
            goto done;
        } else
          goto done;
        break;
    }
  }

  /* Set up stack. */
  if (!setup_stack(esp, file_name))
    goto done;

  /* Start address. */
  *eip = (void (*)(void))ehdr.e_entry;

  success = true;

done:
  /* We arrive here whether the load is successful or not. */
  // Dont close it just yet
  if (!success) {
    if (file != NULL) file_allow_write(file);
    file_close(file);
  }
  if (file_name_copy != NULL) free(file_name_copy);
  return success;
}

/* load() helpers. */

static bool install_page(void* upage, void* kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool validate_segment(const struct Elf32_Phdr* phdr, struct file* file) {
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off)file_length(file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr((void*)phdr->p_vaddr))
    return false;
  if (!is_user_vaddr((void*)(phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable) {
  ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT(pg_ofs(upage) == 0);
  ASSERT(ofs % PGSIZE == 0);

  file_seek(file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) {
    /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
    size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    /* Get a page of memory. */
    uint8_t* kpage = palloc_get_page(PAL_USER);
    if (kpage == NULL)
      return false;

    /* Load this page. */
    if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes) {
      palloc_free_page(kpage);
      return false;
    }
    memset(kpage + page_read_bytes, 0, page_zero_bytes);

    /* Add the page to the process's address space. */
    if (!install_page(upage, kpage, writable)) {
      palloc_free_page(kpage);
      return false;
    }

    /* Advance. */
    read_bytes -= page_read_bytes;
    zero_bytes -= page_zero_bytes;
    upage += PGSIZE;
  }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool setup_stack(void** esp, const char* file_name) {
  uint8_t* kpage;
  bool success = false;
  kpage = palloc_get_page(PAL_USER | PAL_ZERO);
  if (kpage != NULL) {
    success = install_page(((uint8_t*)PHYS_BASE) - PGSIZE, kpage, true);
    if (success) {
      *esp = PHYS_BASE;
      char* file_name_copy = (char*)malloc(sizeof(char) * (strlen(file_name) + 1));
      if (file_name_copy == NULL) {
        palloc_free_page(kpage);
        return false;
      }
      //copy filename into buffer so we can strtok_r
      strlcpy(file_name_copy, file_name, strlen(file_name) + 1);
      char* rest = file_name_copy;
      char* arg = strtok_r(file_name_copy, " ", &rest);

      char* addresses[256] = {NULL};
      uint32_t i = 0;
      while (arg != NULL) {
        *esp -= (strlen(arg) + 1);
        memcpy(*esp, arg, strlen(arg) + 1);
        addresses[i] = *esp;
        arg = strtok_r(NULL, " ", &rest);
        i += 1;
      }

      /*free the copy*/
      free(file_name_copy);

      // Number of arguments is equal to i
      const uint32_t argc = i;

      //add padding here (BYTE ALIGN)
      //4 bytes for each argc and the null pointer on top, plus two more for argc / argv
      const uint32_t total_bytes =
          ((argc + 1) * sizeof(char*)) + sizeof(char**) + sizeof(uint32_t) + (PHYS_BASE - *esp);
      *esp -= (16 - (total_bytes % 16));
      memset(*esp, NULL, (16 - (total_bytes % 16)));

      //add null pointer on stack
      *esp -= sizeof(char*);
      memset(*esp, NULL, sizeof(char*));

      // Do something here
      for (int k = (int)(argc - 1); k >= 0; k--) {
        char* address = addresses[k];
        *esp -= sizeof(char*);
        memcpy(*esp, &address, sizeof(char*));
      }

      //add argv, argc, return address
      const char** argv = (char**)*esp;
      *esp -= sizeof(char**);
      memcpy(*esp, &argv, sizeof(char**));

      *esp -= sizeof(uint32_t);
      memcpy(*esp, &argc, sizeof(uint32_t));

      *esp -= sizeof(uint32_t);
      memset(*esp, NULL, sizeof(uint32_t));

    } else
      palloc_free_page(kpage);
  }
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool install_page(void* upage, void* kpage, bool writable) {
  struct thread* t = thread_current();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page(t->pcb->pagedir, upage) == NULL &&
          pagedir_set_page(t->pcb->pagedir, upage, kpage, writable));
}

/* Returns true if t is the main thread of the process p */
bool is_main_thread(struct thread* t, struct process* p) { return p->main_thread == t; }

/* Gets the PID of a process */
pid_t get_pid(struct process* p) { return (pid_t)p->main_thread->tid; }
