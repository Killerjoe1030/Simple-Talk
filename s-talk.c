#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "list.h"

/* Important Macros */
#define BUFFER_SIZE 1024 // max number of characters

/* Global Variables */
// Mutex for the shared list dedicated to reading in input
pthread_mutex_t mutexRead = PTHREAD_MUTEX_INITIALIZER;
// Mutex for the shared list dedicated to receiving data and outputting it
pthread_mutex_t mutexWrite = PTHREAD_MUTEX_INITIALIZER;
// Condition Variables 
static pthread_cond_t syncOkToSendCondVar = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t syncOkToSendMutex = PTHREAD_MUTEX_INITIALIZER; //mutex for the condition variable

static pthread_cond_t syncOkToWriteCondVar = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t syncOkToWriteMutex = PTHREAD_MUTEX_INITIALIZER;

pthread_t ReadThread, SendThread, ReceiveThread, WriteThread;

int sockfd;
struct addrinfo hints, *servinfo, *p;
int rv;
struct sockaddr_storage their_addr;
socklen_t addr_len;
// Set up their_addr for sending data
struct sockaddr_in their_addr_in;


// For the read input process
// this function will be used to read user input from command line
void * readInput(List * plist){

    ssize_t bytesRead;
    // Need a while loop of some kind
    // need a semphore to block read from editing list while
    // content is being written by other process
    while(1){
        char buffer[BUFFER_SIZE];
        bytesRead = read(STDIN_FILENO,buffer,sizeof(buffer)-1);

        if(bytesRead == -1){
            printf("Something went wrong!");
        } else {
            // Null terminate the string 
            buffer[bytesRead] = '\0';
            //printf("Input string: %s\n", buffer);
            // Lock the mutex before accessing the critical section
            pthread_mutex_lock(&mutexRead);
            // Insert the input into the shared list 
            char *input_data = strdup(buffer);

            List_prepend(plist, (void*)&buffer);
            //printf("test: %s\n", (char*)List_curr(plist));
            // Unlock the mutex since its done accessing the critical section
            pthread_mutex_unlock(&mutexRead);

            // signal the sender thread 
            // this will indicate that there is an item within the shared list thats ready to be sent
            pthread_mutex_lock(&syncOkToSendMutex);
            {
                pthread_cond_signal(&syncOkToSendCondVar);
            }
            pthread_mutex_unlock(&syncOkToSendMutex);
        }
    }
    return NULL;
}

void * sendData(List* plist){

    while(1){

        // wait until signalled by the readInput thread
        pthread_mutex_lock(&syncOkToSendMutex);
        {
            pthread_cond_wait(&syncOkToSendCondVar, &syncOkToSendMutex);
        }
        pthread_mutex_unlock(&syncOkToSendMutex);

        // Extract data from the shared list 
        pthread_mutex_lock(&mutexRead);
        printf("Sent: %s", (char*)List_curr(plist));
        char *data = (char*)List_trim(plist);
        pthread_mutex_unlock(&mutexRead);

        if (*data == '!' && strlen(data) == 2) {
            printf("Session terminated.\n");
            sendto(sockfd, data, strlen(data), 0, (struct sockaddr *) &their_addr_in, sizeof their_addr_in);
            exit(EXIT_SUCCESS);
        }
        
        // send the data to the other user 
        int numbytes;
        
        if ((numbytes = sendto(sockfd, data, strlen(data), 0,
            (struct sockaddr *)&their_addr_in, sizeof their_addr_in)) == -1) {
            perror("sendData: sendto");
            exit(1);
        }
    }
    return NULL;
}

void * receiveData(List* plist){

    while(1){

        char buffer[BUFFER_SIZE];
        int numbytes;

        if ((numbytes = recvfrom(sockfd, buffer, BUFFER_SIZE-1, 0, 
        (struct sockaddr *)&their_addr, &addr_len)) == -1){
            perror("recvfrom"); 
            exit(1);
        }
        // Null terminate the string 
        buffer[numbytes] = '\0';
        printf("Received: %s", buffer);
        // lock the mutex before accessing the critical section 
        pthread_mutex_lock(&mutexWrite);
        // Insert the data into the list containing the data received 
        List_prepend(plist, (void*)&numbytes);
        // unlock the mutex 
        pthread_mutex_unlock(&mutexWrite); 

        // signal the write thread 
        pthread_mutex_lock(&syncOkToWriteMutex);
        {
            pthread_cond_signal(&syncOkToWriteCondVar);
        }
        pthread_mutex_unlock(&syncOkToWriteMutex);

     
        if (strcmp(buffer, "!\n") == 0 && strlen(buffer) == 2) {
            printf("Session terminated.\n");
            exit(EXIT_SUCCESS);
        }

    }  
    return NULL;
}

void * writeOutput(List * plist){

    while(1){

        // wait for the signal from the receiveData process
        pthread_mutex_lock(&syncOkToWriteMutex);
        {
            pthread_cond_wait(&syncOkToWriteCondVar, &syncOkToWriteMutex);
        }
        pthread_mutex_unlock(&syncOkToWriteMutex);
        // Lock the mutex before accessing critical section
        pthread_mutex_lock(&mutexWrite);
        // Unlock the mutex since its done accessing the critical section
        pthread_mutex_unlock(&mutexWrite);
    }
    return NULL;
}


int main(int argc, char *argv[]){
    /* Socket Stuff */
    if (argc != 4) { 
        fprintf(stderr, "Usage: %s  [my port number] [remote machine name/localhost] [remote port number]\n", argv[0]);
        exit(1);
    }
    // argv[1] is my portnumber, argv[2] is hostname, argv[3] is portnumber of the other computer
    // Read input
    int  portNum = atoi(argv[1]);
    char *hostname = argv[2];
    int portNum2 = atoi(argv[3]);
    printf("S-talk Session Initiated\n");

    // Create socket
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Error in socket creation");
        exit(EXIT_FAILURE);
    }

    // Bind socket to port
    // Get info about host address
    struct sockaddr_in my_addr; 
    memset(&my_addr, 0 ,sizeof my_addr);
    my_addr.sin_family = AF_INET; 
    my_addr.sin_port = htons(portNum);
    my_addr.sin_addr.s_addr = INADDR_ANY;

    // binding host socket to port 
    if(bind(sockfd,(struct sockaddr *)&my_addr, sizeof(my_addr)) == -1){
        close(sockfd);
        perror("bind");
        exit(EXIT_FAILURE); 
    }

    // setting up the other address info 
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    if(getaddrinfo(hostname,NULL,&hints,&res) != 0){
        fprintf(stderr, "Error resolving target address\n");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    their_addr_in = *((struct sockaddr_in *)(res->ai_addr));
    their_addr_in.sin_port = htons(portNum2);



    List * SharedListRead = List_create();
    List * SharedListWrite = List_create();
    
    int read, send, receive, write;

    // Create and launch the readInput thread
    read = pthread_create(&ReadThread, NULL, (void *(*)(void *))readInput, SharedListRead);
    if (read != 0) {
        fprintf(stderr, "Error creating readInput thread\n");
        exit(EXIT_FAILURE);
    }

    // Create and launch the sendData thread
    send = pthread_create(&SendThread, NULL, (void *(*)(void *))sendData, SharedListRead);
    if (send != 0) {
        fprintf(stderr, "Error creating sendData thread\n");
        exit(EXIT_FAILURE);
    }

    // Create and launch the receiveData thread
    receive = pthread_create(&ReceiveThread, NULL, (void *(*)(void *))receiveData, SharedListWrite);
    if (receive != 0) {
        fprintf(stderr, "Error creating receiveData thread\n");
        exit(EXIT_FAILURE);
    }

    // Create and launch the writeOutput thread
    write = pthread_create(&WriteThread, NULL, (void *(*)(void *))writeOutput, SharedListWrite);
    if (write != 0) {
        fprintf(stderr, "Error creating writeOutput thread\n");
        exit(EXIT_FAILURE);
    }

    // terminate all threads
    pthread_join(ReadThread, NULL);
    pthread_join(SendThread, NULL);
    pthread_join(ReceiveThread, NULL);
    pthread_join(WriteThread, NULL);

    freeaddrinfo(servinfo);
    freeaddrinfo(res);
    // Close the socket before exiting
    close(sockfd);

    return 0;
}