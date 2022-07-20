#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "userprog/process.h"
#include "threads/vaddr.h"
#include "devices/shutdown.h"
#include "userprog/pagedir.h"
#include "filesys/fd.h"

#define EXIT(code) {\
  f->eax = (uint32_t) code;\
  process_exit(code);\
  return;\
}

#define RETURN(val)\
  {\
    f->eax = (uint32_t) val;\
    return;\
  }

static void syscall_handler(struct intr_frame*);

void syscall_init(void) {
  intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void syscall_handler(struct intr_frame* f UNUSED) {
  uint32_t* args = ((uint32_t*) f->esp);

  /*Return true if address is not a null pointer, is in user space,
  and also points to initialized valid memory. Used for checking
  the args addressed passed in. The arg is expected to be 4 bytes long.*/
  void validate_args(uint32_t address) {
    if ((void*) address == NULL) EXIT(-1);
    for (int i = 0; i < 4; i++) {
      if (
        !is_user_vaddr((const void*)address + i)
        || pagedir_get_page(active_pd(), (const void*)address + i) == NULL
      ) EXIT(-1);
    }
  }

  /*Validates a passed in address to a string from the args array.*/
  void validate_string_arg(uint32_t address) {
    char* str_address = (char*)address;
    for (int i = 0;; i++) {
      if (
        !is_user_vaddr((const void*)str_address + i)
        || pagedir_get_page(active_pd(), (const void*) str_address + i) == NULL
      ) EXIT(-1);
      if (!*(str_address + i)) break;
    }
  }

  /*Validates a passed in address to a buffer from the args array.*/
  void validate_buffer_arg(uint32_t address, unsigned size) {
    for (int i = 0; i < size; i++) {
      if (
        !is_user_vaddr((const void*) address + i)
        || pagedir_get_page(active_pd(), (const void*) address + i) == NULL
      ) EXIT(-1);
    }
  }

  validate_args((uint32_t) args);

  switch (args[0]) {
    case SYS_COMPUTE_E: {
      int n = (int) args[1];
      if (n == NULL || n < 0) EXIT(-1);
      RETURN(sys_sum_to_e(n));
    }
    case SYS_CREATE: {
      const char* filename = (const char*) args[1];
      const unsigned initial_size = (const unsigned) args[2];

      if (filename == NULL) EXIT(-1);
      validate_args((uint32_t) filename);

      RETURN(sys_create(filename, initial_size));
    }
    case SYS_REMOVE: {
      const char* filename = (const char*) args[1];

      if (filename == NULL) EXIT(-1);
      validate_args((uint32_t) filename);

      RETURN(sys_remove(filename));
    }
    case SYS_OPEN: {
      const char* filename = (const char*) args[1];

      if (filename == NULL) EXIT(-1);
      validate_args((uint32_t) filename);

      RETURN(sys_open(filename));
    }
    case SYS_FILESIZE: {
      const int fd = (const int) args[1];

      if (fd >= MAX_FS_OPEN || fd < 0) EXIT(-1);
      switch (fd) {
        case STDIN_FILENO: EXIT(-1);
        case STDOUT_FILENO: EXIT(-1);
        case STDERR_FILENO: EXIT(-1);
        default:
          if(!is_open_fd(fd)) EXIT(-1);
          RETURN(sys_filesize(fd));
      }
    }
    case SYS_READ: {
      const int fd = (const int) args[1];
      char* buffer = (char*) args[2];
      const unsigned size = (const unsigned) args[3];

      if (fd >= MAX_FS_OPEN || fd < 0) EXIT(-1);
      validate_args((uint32_t) buffer);

      switch (fd) {
        case STDOUT_FILENO: EXIT(-1);
        case STDERR_FILENO: EXIT(-1);
        default:
          if(!is_open_fd(fd)) EXIT(-1);
          RETURN(sys_read(fd, buffer, size));
      }
    }
    case SYS_WRITE: {
      const int fd = (const int) args[1];
      const char* buffer = (const char*) args[2];
      const unsigned size = (const unsigned) args[3];

      if (fd >= MAX_FS_OPEN || fd < 0) EXIT(-1);
      validate_args((uint32_t) buffer);

      switch (fd) {
        case STDIN_FILENO: EXIT(-1);
        default:
          if(!is_open_fd(fd)) EXIT(-1);
          RETURN(sys_write(fd, buffer, size));
      }
    }
    case SYS_SEEK: {
      const int fd = (const int) args[1];
      const unsigned position = (const unsigned) args[2];

      if (fd >= MAX_FS_OPEN || fd < 0) EXIT(-1);
      switch (fd) {
        case STDIN_FILENO: EXIT(-1);
        case STDOUT_FILENO: EXIT(-1);
        case STDERR_FILENO: EXIT(-1);
        default:
          if(!is_open_fd(fd)) EXIT(-1);
          sys_seek(fd, position);
          RETURN(NULL);
      }
    }
    case SYS_TELL: {
      const int fd = (const int) args[1];

      if (fd >= MAX_FS_OPEN || fd < 0) EXIT(-1);
      switch (fd) {
        case STDIN_FILENO: EXIT(-1);
        case STDOUT_FILENO: EXIT(-1);
        case STDERR_FILENO: EXIT(-1);
        default:
          if(!is_open_fd(fd)) EXIT(-1);
          RETURN(sys_tell(fd));
      }
    }
    case SYS_CLOSE: {
      const int fd = (const int) args[1];

      if (fd >= MAX_FS_OPEN || fd < 0) EXIT(-1);
      switch (fd) {
        case STDIN_FILENO: EXIT(-1);
        case STDOUT_FILENO: EXIT(-1);
        case STDERR_FILENO: EXIT(-1);
        default:
          if(!is_open_fd(fd)) EXIT(-1);
          sys_close(fd);
          RETURN(NULL);
      }
    }
    case SYS_EXIT: {
      validate_args(&args[1]);
      EXIT(args[1]);
    }
    case SYS_PRACTICE: {
      validate_args(&args[1]);
      int num = (int) args[1];
      num += 1;
      RETURN(num);
    }
    case SYS_HALT: {
      shutdown_power_off();
      return;
    }
    case SYS_EXEC: {
      validate_args(&args[1]);
      validate_string_arg(args[1]);
      pid_t process_id = process_execute((const char*)args[1]);
      if (process_id == TID_ERROR) { //if the shared wait data didin't load
        f->eax = -1;
      } else {
        f->eax = process_id;
      }
      return;
    }
    case SYS_WAIT: {
      validate_args(&args[1]);
      pid_t pid = (pid_t) args[1];
      RETURN(process_wait(pid));
    }
    case SYS_CHDIR: {
      const char* path = (const char*) args[1];

      if (path == NULL) EXIT(-1);
      validate_args((uint32_t) path);
      RETURN(sys_chdir(path));
    }
    case SYS_MKDIR: {
      const char* path = (const char*) args[1];

      if (path == NULL) EXIT(-1);
      validate_args((uint32_t) path);
      RETURN(sys_mkdir(path));
    }
    case SYS_READDIR: {
      const int fd = (const int) args[1];
      char* buffer = (char*) args[2];
      
      if (fd >= MAX_FS_OPEN || fd < 0) EXIT(-1);
      validate_args((uint32_t) buffer);

      switch (fd) {
        case STDIN_FILENO: EXIT(-1);
        case STDOUT_FILENO: EXIT(-1);
        case STDERR_FILENO: EXIT(-1);
        default:
          if(!is_open_fd(fd)) EXIT(-1);
          RETURN(sys_readdir(fd, buffer));
      }
    }
    case SYS_ISDIR: {
      const int fd = (const int) args[1];

      if (fd >= MAX_FS_OPEN || fd < 0) EXIT(-1);

      switch (fd) {
        case STDIN_FILENO: EXIT(-1);
        case STDOUT_FILENO: EXIT(-1);
        case STDERR_FILENO: EXIT(-1);
        default:
          if(!is_open_fd(fd)) EXIT(-1);
          RETURN(sys_isdir(fd));
      }
    }
    case SYS_INUMBER: {
      const int fd = (const int) args[1];
      if (fd >= MAX_FS_OPEN || fd < 0) EXIT(-1);

      switch (fd) {
        case STDIN_FILENO: EXIT(-1);
        case STDOUT_FILENO: EXIT(-1);
        case STDERR_FILENO: EXIT(-1);
        default:
          if(!is_open_fd(fd)) EXIT(-1);
          RETURN(sys_inumber(fd));
      }
    }
    default: EXIT(-1);
  }
}
