#include <string.h> 
#include <sys/socket.h> 
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "utils.h"
#define PORT 8095
#define SA struct sockaddr 

#define TAG "GPS: "

struct gps_data {
    char *data;
    pthread_mutex_t lock;
    sem_t wait_job;
};

static struct gps_data *g_data = 0;

void *gps_client(void *arg){
    
    (void)arg;
    int sockfd;
    struct sockaddr_in servaddr; 
    for( ;; ) {
        sockfd = socket(AF_INET, SOCK_STREAM, 0); 
        if (sockfd == -1) { 
            D(TAG"socket creation failed..."); 
            exit(0); 
        } 
        else
            I(TAG"Socket successfully created.."); 
        bzero(&servaddr, sizeof(servaddr)); 

        // assign IP, PORT 
        servaddr.sin_family = AF_INET; 
        servaddr.sin_addr.s_addr = inet_addr("127.0.0.1"); 
        servaddr.sin_port = htons(PORT); 

        // connect the client socket to server socket 
        if (connect(sockfd, (SA*)&servaddr, sizeof(servaddr)) != 0) { 
            D(TAG"connection with the server failed, reconnect...");
            sleep(1);
        } else { 
            I(TAG"connected to the server..");        
            for( ;; ) {
                sleep(25);
                sem_wait(&g_data->wait_job);
                I(TAG"NEW JOB");
                pthread_mutex_lock(&g_data->lock);
                size_t len = strlen(g_data->data);
                size_t writed = write(sockfd, g_data->data, len);
                I(TAG"write to server=%s, writed=%zd\n",g_data->data, writed);        
                pthread_mutex_unlock(&g_data->lock);
                free(g_data->data);
                if( writed == -1 ) {
                    D(TAG"i/o error, reconnect..");                   
                    break;
                }
            }
        }
    }
    return 0;
}

int update_gps_hw(char *new_data){
    
    if(new_data == 0)
        return 1;
    
    size_t request_sz = strlen(new_data) + 2;  
    
    char *tmp = malloc(request_sz);
    if( tmp == 0 )
        return 1;
    
    memset(tmp, 0, request_sz - 2);
    memcpy(tmp, new_data, strlen(new_data));
    memset(tmp +  strlen(new_data), '\n', 1);
    I(TAG"updated data=%s\n", tmp);
    g_data->data = tmp;
    sem_post(&g_data->wait_job);
    return 0;
}

int init_gps_client(){
    pthread_t thread_id;
    
    g_data = malloc(sizeof(struct gps_data));
    if( g_data == 0 )
        error("mem alloc");
    
    g_data->data = 0;
    pthread_mutex_init(&g_data->lock, 0);
    sem_init(&g_data->wait_job, 0, 0);
    
    return pthread_create(&thread_id, NULL, gps_client, NULL);
}