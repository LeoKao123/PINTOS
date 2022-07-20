#include "path.h"
#include "inode.h"
/**
 * @brief This file contains helper functions that may be useful
 * 
 */


/* Extracts a file name part from *SRCP into PART, and updates *SRCP so that the
next call will return the next file name part. Returns 1 if successful, 0 at
end of string, -1 for a too-long file name part. */
static int get_next_part(char part[NAME_MAX + 1], const char** srcp) {
	const char* src = *srcp;
	char* dst = part;
	/* Skip leading slashes. If it's all slashes, we're done. */
	while (*src == '/') src++;
	if (*src == '\0') return 0;
	/* Copy up to NAME_MAX character from SRC to DST. Add null terminator. */
	while (*src != '/' && *src != '\0') {
		if (dst < part + NAME_MAX) *dst++ = *src;
		else return -1;
		src++;
	}
	*dst = '\0';
	/* Advance source pointer. */
	*srcp = src;
	return 1;
}

/**
 * @brief Get the directory of the path, returns NULL if no directory exists
 * You must close the directory afterwards
 * 
 * @param path 
 * @return struct dir* 
 */
struct dir* path_dir(const char* path) {
	struct thread* thread = thread_current();
	char* basename = path_basename(path);
	if (basename == NULL) return NULL;

	struct dir* cwd = thread->cwd;
	struct dir* dir = (cwd == NULL || path == NULL || path[0] == '/') ? dir_open_root() : dir_reopen(cwd);

	if (dir == NULL) {
		free(basename);
		return NULL;
	}

	char* path_dirname_copy = NULL;
	char* path_dirname = (char*) calloc(strlen(path) + 1 - strlen(basename), sizeof(char));
	if (path_dirname == NULL) {
		dir_close(dir);
		free(basename);
		return NULL;
	}
	memcpy(path_dirname, path, strlen(path) - strlen(basename));

	path_dirname_copy = path_dirname;

	char part[NAME_MAX + 1] = {0};

	struct inode* inode;

	int res = 0;

	if (strlen(path_dirname) == 0) {
		free(path_dirname_copy);
		free(basename);
		return dir;
	}

	while(1) {
		res = get_next_part(part, (const char**) &path_dirname);
		if (res == -1) {
			free(path_dirname_copy);
			free(basename);
			dir_close(dir);
			return NULL;
		}
		if (res == 0) {
			free(path_dirname_copy);
			free(basename);
			return dir;
		}
		if (!dir_lookup(dir, part, &inode)) {
			free(path_dirname_copy);
			free(basename);
			dir_close(dir);
			return NULL;
		}

		struct dir* temp_dir = dir_open(inode);

		if (temp_dir == NULL) {
			free(path_dirname_copy);
			free(basename);
			dir_close(dir);
			return NULL;
		}

		dir_close(dir);
		dir = temp_dir;
	}

}

/**
 * @brief Returns the basename of the path
 * You MUST free data afterwards
 * Ex: /main/nested/file -> file
 * 
 * @param path
 * @param part 
 * @return true 
 * @return false 
 */
char* path_basename(const char* path) {
	char* part = (char*) calloc(NAME_MAX + 1, sizeof(char));

	if (part == NULL) {
		return NULL;
	}

  char* path_copy = (char*) malloc(strlen(path) + 1);
  if (path_copy == NULL) {
		free(part);
		return NULL;
	}

	strlcpy(path_copy, path, strlen(path) + 1);

	int res = 0;
	while(1) {
		res = get_next_part(part, (const char**) &path);
		if (res == -1) {
				free(part);
		    free(path_copy);
		    return NULL;
		}
		if (res == 0) {
		    free(path_copy);
		    return part;
		}
	}
}

/**
 * @brief Returns the inode of the path, if one doesn't exist, then return NULL
 * You MUST close the inode afterwards.
 * 
 * @param path 
 * @return struct inode* 
 */
struct inode* path_inode(const char* path) {
	struct thread* thread = thread_current();
	struct inode* inode = NULL;

	char* name = path_basename(path);
	if (name == NULL) return NULL;

	struct dir* dir = path_dir(path);
	if (dir == NULL) goto done;

	if (strlen(name) == 0) inode = inode_reopen(dir_get_inode(dir));
	else dir_lookup((const struct dir*) dir, name, &inode);

	dir_close(dir);
	done:
		free(name);
		return inode;
}

/**
 * @brief Checks to see if the path exists. It is recommended to use path_inode instead
 * 
 * @param path 
 * @return true 
 * @return false 
 */
bool path_exists(const char* path) {
	struct thread* thread = thread_current();

	struct inode* inode = path_inode(path);

	if (inode == NULL) return false;

	inode_close(inode);

	return true;
}

/**
 * @brief Checks to see if the directory is empty
 * 
 * @param dir 
 * @return true 
 * @return false 
 */
bool path_is_empty_dir(const struct dir* dir) {
	struct dir* clone = dir_reopen(dir);

	char name[NAME_MAX + 1];

	while(dir_readdir(clone, name)) {
		if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
		dir_close(clone);
		return false;
	}

	dir_close(clone);

	return true;
}

/**
 * @brief Currently this is broken
 * 
 * @param dir 
 * @param indent 
 */
static void print_full_dir(const struct dir* dir, int indent) {
	char name[NAME_MAX + 1];
	while (dir_readdir(dir, name)) {
		for (int i = 0; i < indent; i++) printf("\t");

		struct inode* inode = NULL;
		printf("%s\n", name);
		if (dir_lookup((const struct dir*) dir, name, inode)) {
			if (inode_type(inode) == INODE_DIRECTORY) {
				print_full_dir(dir_open(inode), indent + 1);
			}
		}
	}
}

void print_dir() {
	printf("\n/\n");
	print_full_dir(dir_open_root(), 1);
	printf("\n");
}