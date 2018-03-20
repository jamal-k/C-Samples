#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

/* Read a user id and password from standard input, 
   - create a new process to run the validate program
   - send 'validate' the user id and password on a pipe, 
   - print a message 
        "Password verified" if the user id and password matched, 
        "Invalid password", or 
        "No such user"
     depending on the return value of 'validate'.
*/

#define VERIFIED "Password verified\n"
#define BAD_USER "No such user\n"
#define BAD_PASSWORD "Invalid password\n"
#define OTHER "Error validating password\n"


int main(void) {
    char userid[10];
    char password[10];

    /* Read a user id and password from stdin */
    printf("User id:\n");
    scanf("%s", userid);
    printf("Password:\n");
    scanf("%s", password);
    
    int fd[2];
    
    if (pipe(fd) == -1) {
    	perror("pipe");
    	exit(1);
    }
    
    int result = fork();
    if (result < 0) {
    	perror("fork");
    	exit(1);
    } else if (result == 0) {
    	close(fd[1]);
    	dup2(fd[0], fileno(stdin));
    	//char *np = NULL;
    	execl("./validate", "validate", NULL);
    	perror("exec");
    	exit(1);
    } else {
    	close(fd[0]);
    	if (write(fd[1], userid, sizeof(userid)) == -1) {
    		perror("write to pipe");
    	}
    	if (write(fd[1], password, sizeof(userid)) == -1) {
    		perror("write to pipe");
    	}
    	close(fd[1]);
    }
    
    int status;
    if (wait(&status) != -1) {
    	if (WIFEXITED(status)) {
	    	if (WEXITSTATUS(status) == 0) {
	    		printf(VERIFIED);
	    	} else if (WEXITSTATUS(status) == 2) {
	    		printf(BAD_PASSWORD);
	    	} else if (WEXITSTATUS(status) == 3) {
	    		printf(BAD_USER);
	    	} else {
	    		printf(OTHER);
	    	}
    	} else {
	    	printf(OTHER);
	    }
    }
    
    return 0;
}
