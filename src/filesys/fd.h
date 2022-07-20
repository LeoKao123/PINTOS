#ifndef FD_H
#define FD_H

#include <stdio.h>
#include <string.h>
#include "filesys/path.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/directory.h"
#include "devices/input.h"
#include "threads/thread.h"
#include "threads/malloc.h"

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define MAX_FS_OPEN 128

struct FILE {
	struct dir* dir;
	struct file* file;
};

typedef struct FILE FILE;

bool is_open_fd(int fd);
bool sys_isdir(int fd);
bool sys_create(const char* filename, const unsigned initial_size);
bool sys_remove(const char* filename);
int sys_open(const char* filename);
int sys_filesize(int fd);
int sys_read(int fd, char* buffer, const unsigned size);
int sys_write(int fd, const char* buffer, const unsigned size);
void sys_seek(int fd, const unsigned position);
unsigned sys_tell(int fd);
void sys_close(int fd);
void close_all(void);

bool sys_chdir(const char* path);
bool sys_mkdir(const char* path);
bool sys_readdir(int fd, char* name);
int sys_inumber(int fd);

#endif