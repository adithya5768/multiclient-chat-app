#include <stdio.h>
#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <signal.h>


int sockfd, portno=5000, pid;


/*Function to handle ^C*/
void sigCHandler() 
{ 
	//WRITE YOUR CODE HERE
    char buffer[256] = "/quit\n";
    write(sockfd,buffer,sizeof(buffer));
    if (pid)
        kill(pid, SIGQUIT);
	exit(0);
} 

/*Function to handle ^Z*/
void sigZhandler() 
{ 
	//WRITE YOUR CODE HERE
    char buffer[256] = "/quit\n";
    write(sockfd,buffer,sizeof(buffer));
    if (pid)
        kill(pid, SIGQUIT);
	exit(0);
}

void error(char *msg)
{
  perror(msg);
  exit(0);
}

void SendMessages()
{
    char buffer[256];
    printf("Enter the commands and type /quit to quit\n");
    while(1)
    {
        bzero(buffer,256);
        fgets(buffer,256,stdin);
        write(sockfd,buffer,sizeof(buffer));
        if (strcmp(buffer, "/quit\n") == 0)
        {
            kill(pid, SIGQUIT);
            break;
        }
    }
}

void ReceiveMessages()
{
    char buffer[256];
    while(1)
    {
        bzero(buffer,256);
        recv(sockfd, buffer, sizeof(buffer), 0);
        printf("%s\n",buffer);
    }
}

int main(int argc,char *argv[])
{
    signal(SIGINT, sigCHandler);  // handles ^C
	signal(SIGTSTP, sigZhandler);    //handles ^Z
 
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct hostent *server;
    server = gethostbyname("127.0.0.1");
    if (server == NULL)
    {
        fprintf(stderr,"ERROR, no such host");
        exit(0);
    }
    struct sockaddr_in serv_addr;
    bzero((char *) &serv_addr, sizeof(serv_addr)); // initializes buffer
    serv_addr.sin_family = AF_INET; // for IPv4 family
    bcopy((char *)server->h_addr, (char *) &serv_addr.sin_addr.s_addr, server->h_length);
    serv_addr.sin_port = htons(portno); //defining port number
    if (connect(sockfd,(struct sockaddr *) &serv_addr,sizeof(serv_addr)) < 0)
    {
         error("ERROR connecting");
    }
    
    pid = fork();
    if (pid == 0)
    {
        close(0);
        ReceiveMessages();
    }
    else
    {
        SendMessages();
    }
    
    close(sockfd);
    return 0;
}