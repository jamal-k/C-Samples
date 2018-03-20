#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include "ftree.h"
#include "hash.h"

#define MAX_ENTS 100

int INITIAL_FLAG = 1; // 1 for initial call of copy_ftree, 0 afterwards

/*
 * Generates path, given parent and child in file tree
 */
void set_path(char* path, char *parent, char *child) {
	strncpy(path, parent, PATH_MAX);
	path[strlen(parent)] = '/';
	strncat(path, child, PATH_MAX - strlen(path));
}

/*
 * Overwrite dest file with src file
 */
void overwrite(FILE *src, FILE *dest, char *dest_path, mode_t perms) {
	//update permissions
	if (chmod(dest_path, perms) < 0) {
		perror("chmod");
		exit(-1);
	}
	
	char curr_byte;
	
	while(fread(&curr_byte, 1, 1, src) == 1) { // read one byte at a time from src
		if (fwrite(&curr_byte, 1, 1, dest) != 1) { // write one byte at a time to dest
			fprintf(stderr, "File write error\n");
			exit(-1);
		}
		if (ferror(src) != 0) {
			fprintf(stderr, "File stream error\n");
			exit(-1);
		}
	}
}

/*
 * Copy file from src_path to dest_path, overwriting if necessary
 */
void copy_file(char *src_path, char *dest_path, mode_t perms) {
	
	FILE *src;
	src = fopen(src_path, "r");
	if (src == NULL) {
		fprintf(stderr, "Error opening file\n");
		exit(-1);
	}
	
	// seek to final byte of file to determine its size
	if (fseek(src, 0L, SEEK_END) < 0) {
		perror("fseek");
		exit(-1);
	}
	long src_size = ftell(src);
	rewind(src); // reset stream position
	
	FILE *dest;
	dest = fopen(dest_path, "r");
	if (dest == NULL) {
		dest = fopen(dest_path, "w");
		if (dest == NULL) {
			fprintf(stderr, "Error opening file\n");
			exit(-1);
		}
		overwrite(src, dest, dest_path, perms); // overwrite into new file
	} else {
		//check size, hash, create copy if needed
		if (fseek(dest, 0L, SEEK_END) < 0) {
			perror("fseek");
			exit(-1);
		}
		long dest_size = ftell(dest);
		rewind(dest);
		
		// overwrite if size or hash differ
		if (src_size != dest_size) {
			dest = fopen(dest_path, "w");
			if (dest == NULL) {
				fprintf(stderr, "Error opening file\n");
				exit(-1);
			}
			overwrite(src, dest, dest_path, perms);
		} else if (hash(src) != hash(dest)) {
			// close and open streams after they're hashed
			if (fclose(src) == EOF) {
				perror("fclose");
				exit(-1);
			}
			if (fclose(dest) == EOF) {
				perror("fclose");
				exit(-1);
			}
			src = fopen(src_path, "r");
			if (src == NULL) {
				fprintf(stderr, "Error opening file\n");
				exit(-1);
			}
			dest = fopen(dest_path, "w");
			if (dest == NULL) {
				fprintf(stderr, "Error opening file\n");
				exit(-1);
			}
			overwrite(src, dest, dest_path, perms);
		}
	}
}

/* 
 * Function for copying a file tree rooted at src to dest.
 * Returns < 0 on error. The magnitude of the return value
 * is the number of processes involved in the copy and is
 * at least 1.
 */
int copy_ftree(const char *src, const char *dest) {
    
    int processes = 1;
    
    // path for copy of src directory in dest
   	char src_copy_path[PATH_MAX];
   	if (INITIAL_FLAG) {
   		set_path(src_copy_path, (char *) dest, basename((char *) src));
   	} else {
   		set_path(src_copy_path, (char *) dest, "");
   	}
   	
   	// make new src directory if one not already present
   	DIR *src_copy;
   	src_copy = opendir(src_copy_path);
	if (src_copy == NULL) {
   		if (mkdir(src_copy_path, S_IRUSR || S_IWUSR || S_IXUSR) < 0) {
   			perror("mkdir");
   			exit(-1);
   		}
   	}
	
	struct stat src_stat;
	if (lstat(src, &src_stat) == -1) {
   		perror("lstat");
   		exit(-1);
   	}
   	
   	//update src_copy permissions
	if (chmod(src_copy_path, src_stat.st_mode & 0777) < 0) {
		perror("chmod");
		exit(-1);
	}
    
    // array of entries in src directory
    struct dirent **src_ents = malloc(sizeof(struct dirent *) * MAX_ENTS);
    
    DIR *src_dir = opendir(src);
	if (src_dir == NULL) {
		perror("opendir");
	    exit(-1);
	}
	
	struct dirent *src_ent;
	int i = 0;
	
	// populate array of entries
	while ((src_ent = readdir(src_dir))) {
		if (src_ent->d_name[0] != '.') {
			src_ents[i] = src_ent;
			i++;
		}
	}

	if (closedir(src_dir) < 0) {
		perror("closedir");
	    exit(-1);
	}
	
	struct stat curr_stat;
	char curr_path[PATH_MAX], new_path[PATH_MAX]; // new paths for src and dest in recursive call
	int r; // for fork parent/child checking
	int status; // exit status for child processes
	DIR *sub_dir; // subdirectory for src dir entries
	
	for (int s = 0; s < i; s++) {
		
		if (src_ents[s]->d_name[0] != '.') {
		
		set_path(curr_path, (char *) src, src_ents[s]->d_name);
    	if (lstat(curr_path, &curr_stat) == -1) {
    		perror("lstat");
    		exit(-1);
    	}
    	
    	set_path(new_path, src_copy_path, src_ents[s]->d_name);
    	
    	
    	// perform correct copy based on type of src entry
    	switch(curr_stat.st_mode & S_IFMT) {
    	
    	case S_IFREG:
    	
    		copy_file(curr_path, new_path, curr_stat.st_mode & 0777);
    		break;
    	
    	case S_IFDIR:
    		
    		// recursive call reached, so no longer initial copy
    		INITIAL_FLAG = 0;
    		// make copy of subdirectory
    		sub_dir = opendir(new_path);
    		if (sub_dir == NULL) {
    			if (mkdir(new_path, curr_stat.st_mode & 0777) < 0) {
    				perror("mkdir");
    				exit(-1);
    			}
    		}
    		    		
    		r =  fork();
    		
    		if (r < 0) {
    			perror("fork");
    			exit(-1);
    		} else if (r == 0) {
    			// perform recursive copy and set exit status of child process appropriately
    			exit(copy_ftree(curr_path, new_path));
    		} else if (r > 0) {
    			
    			if (wait(&status) < 0) {
    				fprintf(stderr, "Wait for child process failed");
    				exit(-1);
    			}
    			if (WIFEXITED(status)) {
    				if (status < 0) {
    					exit(-1 * processes); // child process encountered error
    				}
    				else {
    					processes += WEXITSTATUS(status); // update process count
    				}
    			}
    		}
    		break;
    	
    	default:
    		break;
    	}
		}
	}
	free(src_ents);
    return processes;
}
