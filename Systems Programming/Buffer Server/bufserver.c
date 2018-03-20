#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>

#ifndef PORT
  #define PORT 50186
#endif

int setup(void) {
  int on = 1, status;
  struct sockaddr_in self;
  int listenfd;
  if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
    perror("socket");
    exit(1);
  }

  // Make sure we can reuse the port immediately after the
  // server terminates.
  status = setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
                      (const char *) &on, sizeof(on));
  if(status == -1) {
    perror("setsockopt -- REUSEADDR");
  }

  self.sin_family = AF_INET;
  self.sin_addr.s_addr = INADDR_ANY;
  self.sin_port = htons(PORT);
  memset(&self.sin_zero, 0, sizeof(self.sin_zero));  // Initialize sin_zero to 0

  printf("Listening on %d\n", PORT);

  if (bind(listenfd, (struct sockaddr *)&self, sizeof(self)) == -1) {
    perror("bind"); // probably means port is in use
    exit(1);
  }

  if (listen(listenfd, 5) == -1) {
    perror("listen");
    exit(1);
  }
  return listenfd;
}

/*Search the first inbuf characters of buf for a network newline ("\r\n").
  Return the location of the '\r' if the network newline is found,
  or -1 otherwise.
*/

int find_network_newline(const char *buf, int inbuf) {
  
  for (int i = 0; i < inbuf - 1; i++) {
      if (buf[i] == '\r' && buf[i+1] == '\n') {
          return i;
      }
  }

  return -1; // return the location of '\r' if found
}

int main() {
  int listenfd;
  int fd, nbytes;
  char buf[30];
  int inbuf; // how many bytes currently in buffer?
  int room; // how much room left in buffer?
  char *after; // pointer to position after the (valid) data in buf
  int where; // location of network newline

  struct sockaddr_in peer;
  socklen_t socklen;

  listenfd = setup();
  while (1) {
    socklen = sizeof(peer);
	
    if ((fd = accept(listenfd, (struct sockaddr *)&peer, &socklen)) < 0) {
      perror("accept");

    } else {
      printf("New connection on port %d\n", ntohs(peer.sin_port));

      // Receive messages
      inbuf = 0;          // buffer is empty; has no bytes
      room = sizeof(buf); // room == capacity of the whole buffer
      after = buf;        // start writing at beginning of buf

      while ((nbytes = read(fd, after, room)) > 0) {
        // update inbuf
        inbuf += nbytes;
        
        where = find_network_newline(buf, inbuf);
        
        if (where >= 0) { // have a full line

          // output the full line, not including the "\r\n"
          // put a '\0' in the correct place first
          // (Replace the "\r\n" with appropriate characters so the 
          // message prints correctly to stdout.)
          
          buf[where] = '\n';
          buf[where + 1] = '\0';
          
          printf("Next message: %s", buf);          
          
          // update inbuf and remove the full line from the buffer
          
          inbuf -= (where + 2);
          for (int i = 0; i < inbuf; i++) {
              buf[i] = buf[i + where + 2];
          }
          for (int j = inbuf; j < 30; j++) {
              buf[j] = '\0';
          }
        }
		
        // update room and after, in preparation for the next read
        room = 30 - inbuf;
        after = buf + inbuf;
       
      }
      close(fd);
    }
  }
  return 0;
}
   
