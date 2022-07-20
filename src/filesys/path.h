#ifndef PATH_H
#define PATH_H

#include <string.h>
#include <debug.h>
#include "directory.h"
#include "threads/malloc.h"
#include "threads/thread.h"

struct dir* path_dir(const char* path);
char* path_basename(const char* path);
struct inode* path_inode(const char* path);
bool path_exists(const char* path);
bool path_is_empty_dir(const struct dir* dir);
void print_dir();

#endif