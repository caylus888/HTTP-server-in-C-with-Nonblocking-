#include<stdio.h>
#include<sys/socket.h> // using them fron unix wala system(wsl)
#include<netinet/in.h>//same
#include<stdlib.h>
#include<string.h>
#include<unistd.h>
//#include<fnctl.h> // setting mr socket to non blocking

void pp();
/*     decide later   *///dev.to/jeffreythecoder/how-i-built-a-simple-http-server-from-scratch-using-c-739 this also !

struct serv{
    int sdomain;//int sdomain,sserver,sprotocol,sport,sbacklog;int socket;
    int sserver;
    int sprotocol;
    int sport;
    int sbacklog ; 
    unsigned long s_interface; 
       struct sockaddr_in add;
    void (*pp)();
    int socketfd,bindfd,listenfd;
};
 
//struct serv serv_construct(int sdomain, int sserver, int sprotocol,int sport,int sbacklog ,unsigned long s_interface , void (*pp)(void)); for lata
 
struct serv serv_construct(int s_domain, int s_server, int s_protocol,int s_port,int s_backlog ,unsigned long s_interface , void (*s_pp)())
{ 
   

    printf("oouuu!\n");
    struct serv server;
    server.sdomain = s_domain;
    server.sserver = s_server; 
    server.s_interface = s_interface;   
    server.sprotocol = s_protocol; 
    server.sport = s_port;
    server.sbacklog =  s_backlog;
    // puting suff from  line 9 wala constructor into line 18 wala constructor 
    
       server.add.sin_family = s_domain;
       server.add.sin_port = htons(s_port); // host data to ( network byte order ) 
       server.add.sin_addr.s_addr = htonl(s_interface);//same

    server.socketfd = socket(s_domain, s_server,s_protocol); //creates socket connection to network 
    int aalu = sizeof(server.add);
   if(server.socketfd < 0) // correction made
   {
      perror("---Failed to connect the socket---\n");
      exit(1);
   }
    server.bindfd = bind(server.socketfd,(const struct sockaddr *)&(server.add),aalu);
   if ( server.bindfd < 0) //binds the socket to network
   {
      perror("Failed to bind the socket\n");
      exit(1);
   }
   server.listenfd = listen(server.socketfd,server.sbacklog);
   if ( server.listenfd < 0) // waits for incoming connection 
   {
      perror("Failed to start listening\n");
      exit(1);
   }

server.pp = s_pp;

return server;
} 
void pp(struct serv *server)
   { printf("wating\n");
    char buffer[40000];
    int alen = sizeof(server->add);
      int nsock = accept(server->socketfd,(struct sockaddr *)&server->add,(socklen_t *)&alen);
      read(nsock,buffer,40000);
      printf("%s\n",buffer);
      const char *msg =
      "HTTP/1.1 200 OK\r\n"
      "Date: Mon, 27 Jul 2023 12:34:56 GMT\r\n"
      "Content-Type: text/plain\r\n"
      "Content-Length: 13\r\n"
      "\r\n"
      "Hello, World!";

                  write(nsock,msg,strlen(msg));
                  close(nsock);
                  close(server->socketfd);
                  close(server->bindfd);
                  close(server->listenfd);
   }  

int main(){

  //struct serv serv_construct(int sdomain, int sserver, int sprotocol,int sport,int sbacklog ,unsigned long s_interface , void (*pp)(void));
    struct serv server = serv_construct(AF_INET, SOCK_STREAM,0,6969,10,INADDR_ANY,pp);
    server.pp(&server);

} 


