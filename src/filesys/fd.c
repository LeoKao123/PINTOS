#include "fd.h"
#include "inode.h"
#include "free-map.h"
#include "userprog/process.h"

static bool is_file(int fd) {
	return (
		fd != STDIN_FILENO
		&& fd != STDOUT_FILENO
		&& fd != STDERR_FILENO
		&& is_open_fd(fd)
	);
}

static FILE* get_file(int fd) {
	ASSERT(fd != STDIN_FILENO);
	ASSERT(fd != STDOUT_FILENO);
	ASSERT(fd != STDERR_FILENO);
	ASSERT(is_open_fd(fd));
	struct process* pcb = thread_current()->pcb;
	return pcb->open_files[fd];
}

bool is_open_fd(int fd) {
	struct process* pcb = thread_current()->pcb;
	return pcb->open_files[fd] != NULL;
}

bool sys_isdir(int fd) {
	struct process* pcb = thread_current()->pcb;
	return pcb->open_files[fd]->dir != NULL;
}

bool sys_create(const char* filename, const unsigned initial_size) {
	struct process* pcb = thread_current()->pcb;
	lock_acquire(&pcb->fd_lock);
	bool result = filesys_create(filename, initial_size);
	lock_release(&pcb->fd_lock);
	return result;
}

bool sys_remove(const char* filename) {
	struct thread* thread = thread_current();
	struct process* pcb = thread->pcb;
	lock_acquire(&pcb->fd_lock);

	struct inode* inode = path_inode(filename);

	bool result = false;

	if (inode == NULL) {
		lock_release(&pcb->fd_lock);
		return false;
	}

	switch (inode_type(inode)) {
		case INODE_FILE: {
			inode_close(inode);
			result = filesys_remove(filename);
			break;
		}
		case INODE_DIRECTORY: {
			struct dir* dir = dir_open(inode);
			result = path_is_empty_dir(dir);
			if (inode_get_inumber(inode) == ROOT_DIR_SECTOR) result = false;
			if (thread->cwd != NULL && inode_get_inumber(dir_get_inode(thread->cwd)) == inode_get_inumber(inode)) result = false;
			dir_close(dir);
			if (result) result = filesys_remove(filename);
			break;
		}
		default: {
			inode_close(inode);
			break;
		}
	}

	lock_release(&pcb->fd_lock);
	return result;
}

int sys_open(const char* filename) {
	if (strlen(filename) == 0) return -1;
	struct process* pcb = thread_current()->pcb;

	lock_acquire(&pcb->fd_lock);

	if (pcb->open_files_cnt >= MAX_FS_OPEN) {
		lock_release(&pcb->fd_lock);
		return -1;
	}

	int fd = pcb->next_open_fd;

	while(pcb->open_files[fd] != NULL) {
		fd = (fd + 1) % MAX_FS_OPEN;
		if (fd <= STDERR_FILENO) fd = STDERR_FILENO + 1;
	}

	FILE* file = (FILE*) malloc(sizeof(FILE));

	if (file == NULL) {
		lock_release(&pcb->fd_lock);
		return -1;
	}

	file->file = NULL;
	file->dir = NULL;

	struct inode* inode = path_inode(filename);

	if (inode == NULL) {
		free(file);
		fd = -1;
	} else {
		switch (inode_type(inode)) {
			case INODE_FILE: {
				file->file = file_open(inode);

				if (file->file == NULL) {
					free(file);
					fd = -1;
					break;
				}

				pcb->open_files[fd] = file;
				pcb->open_files_cnt++;
				break;
			}
			case INODE_DIRECTORY: {
				file->dir = dir_open(inode);

				if (file->dir == NULL) {
					free(file);
					fd = -1;
					break;
				}

				pcb->open_files[fd] = file;
				pcb->open_files_cnt++;
				break;
			}
			default: {
				free(file);
				inode_close(inode);
				fd = -1;
			}
		}
	}

	lock_release(&pcb->fd_lock);

	return fd;
}

int sys_filesize(int fd) {
	struct process* pcb = thread_current()->pcb;

	lock_acquire(&pcb->fd_lock);

	FILE* file = get_file(fd);
	int filesize = file_length(file->file);

	lock_release(&pcb->fd_lock);
	
	return filesize;
}

int sys_read(int fd, char* buffer, const unsigned size) {
	ASSERT(fd != STDOUT_FILENO);
	ASSERT(fd != STDERR_FILENO);

	struct process* pcb = thread_current()->pcb;

	lock_acquire(&pcb->fd_lock);

	int bytes_read = 0;
	if (fd == STDIN_FILENO) {
		for (uint32_t i = 0; i < size; i++) buffer[i] = input_getc();
		bytes_read = (int) size;
	} else {
		if (sys_isdir(fd)) {
			lock_release(&pcb->fd_lock);
			return -1;
		}
	
		FILE* file = get_file(fd);

		bytes_read = (int) file_read(file->file, buffer, size);
	}

	lock_release(&pcb->fd_lock);

	return bytes_read;
}

int sys_write(int fd, const char* buffer, const unsigned size) {
	ASSERT(fd != STDIN_FILENO);

	struct process* pcb = thread_current()->pcb;

	lock_acquire(&pcb->fd_lock);

	int bytes_written = 0;
	if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
		unsigned chunk_write = 256;
		for (unsigned i = 0; i < size; i += 256) {
			chunk_write = 256;
			if (i + 256 > size) chunk_write = size - i;
			putbuf((char*) (buffer + i * 256), chunk_write);
		}
		bytes_written = (int) size;
	} else {
		if (sys_isdir(fd)) {
			lock_release(&pcb->fd_lock);
			return -1;
		}

		FILE* file = get_file(fd);

		bytes_written = (int) file_write(file->file, buffer, size);
	}

	lock_release(&pcb->fd_lock);

	return bytes_written;
}

void sys_seek(int fd, const unsigned position) {
	ASSERT(fd != STDIN_FILENO);
	ASSERT(fd != STDOUT_FILENO);
	ASSERT(fd != STDERR_FILENO);
	ASSERT(is_open_fd(fd));

	struct process* pcb = thread_current()->pcb;

	lock_acquire(&pcb->fd_lock);

	FILE* file = get_file(fd);
	file_seek(file->file, position);

	lock_release(&pcb->fd_lock);
}

unsigned sys_tell(int fd) {
	ASSERT(fd != STDIN_FILENO);
	ASSERT(fd != STDOUT_FILENO);
	ASSERT(fd != STDERR_FILENO);
	ASSERT(is_open_fd(fd));

	struct process* pcb = thread_current()->pcb;

	lock_acquire(&pcb->fd_lock);

	FILE* file = get_file(fd);
	unsigned position = (unsigned) file_tell(file->file);

	lock_release(&pcb->fd_lock);

	return position;
}

void sys_close(int fd) {
	ASSERT(fd != STDIN_FILENO);
	ASSERT(fd != STDOUT_FILENO);
	ASSERT(fd != STDERR_FILENO);
	ASSERT(is_open_fd(fd));

	struct process* pcb = thread_current()->pcb;

	lock_acquire(&pcb->fd_lock);

	FILE* file = get_file(fd);
	if (sys_isdir(fd)) dir_close(file->dir);
	else file_close(file->file);
	free(file);
	pcb->open_files[fd] = NULL;
	pcb->open_files_cnt--;

	lock_release(&pcb->fd_lock);
}

void close_all(void) {
	struct process* pcb = thread_current()->pcb;

	for (int fd = 3; fd < MAX_FS_OPEN; fd++) {
		if (pcb->open_files[fd] != NULL) {
			file_close(pcb->open_files[fd]->file);
			free(pcb->open_files[fd]);
			pcb->open_files[fd] = NULL;
		}
	}
	pcb->open_files_cnt = 3;

}

bool sys_chdir(const char* path) {
	struct thread* thread = thread_current();
	struct inode* inode = path_inode(path);
	struct dir* dir = dir_open(inode);

	if (dir == NULL) return false;

	dir_close(thread->cwd);
	thread->cwd = dir;

	return true;
}

bool sys_mkdir(const char* path) {
	if (strlen(path) == 0) return false;

	char* name = path_basename(path);
	if (name == NULL) return false;

	struct dir* parent_dir = path_dir(path);
	if (parent_dir == NULL) {
		free(name);
		return false;
	}

	struct inode* parent_inode = dir_get_inode(parent_dir);
	int parent_inumber = inode_get_inumber(parent_inode);

	block_sector_t inode_sector = 0;

	bool success = (
		parent_dir != NULL
		&& free_map_allocate(1, &inode_sector)
		&& dir_create(inode_sector, parent_inumber, 16)
		&& dir_add(parent_dir, name, inode_sector)
	);

	if (
		!success
		&& inode_sector != 0
	) free_map_release(inode_sector, 1);

	dir_close(parent_dir);
	free(name);

	return success;
}

bool sys_readdir(int fd, char* name) {
	ASSERT(is_file(fd));
	ASSERT(sys_isdir(fd));
	struct process* pcb = thread_current()->pcb;

	lock_acquire(&pcb->fd_lock);

	FILE* file = get_file(fd);
	struct dir* dir = file->dir;

	bool result = false;

	do {
		result = dir_readdir(dir, name);
	} while(result && (strcmp(name, ".") == 0 || strcmp(name, "..") == 0));

	lock_release(&pcb->fd_lock);

	return result;
}

int sys_inumber(int fd) {
	is_file(fd);
	struct process* pcb = thread_current()->pcb;

	lock_acquire(&pcb->fd_lock);

	FILE* file = get_file(fd);
	int inumber = inode_get_inumber(sys_isdir(fd) ? dir_get_inode(file->dir) : file_get_inode(file->file));

	lock_release(&pcb->fd_lock);

	return inumber;
}