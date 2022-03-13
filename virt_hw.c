#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>

#include "modem_driver.h"
#include "utils.h"
#include "gps_client.h"
#include "console.h"

#define MODEM_PORT 8090
#define COMMAND_PORT 5545
#define PHONE_NUM 5554

#define BUFF_SZ 4096

#define SA struct sockaddr

static void *modem_listener(void *arg) {
    int sockfd, connfd;
    ModemDriver *driver = (ModemDriver *)arg;
    struct sockaddr_in servaddr = {0}, cli = {0};
    socklen_t len = sizeof(struct sockaddr_in);

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1)
        error("Err: socket creation failed...");

#ifdef BUILD_HOST
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0)
        error("Err: setsockopt(SO_REUSEADDR) failed");
#else 
    int enable = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0)
        error("Err: setsockopt(SO_REUSEADDR) failed");
#endif

    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(MODEM_PORT);

    if ((bind(sockfd, (SA*) & servaddr, sizeof (servaddr))) != 0)
        error("Err: failed to bind socket");

    if ((listen(sockfd, 8)) != 0)
        error("Err: socket listen failed");

    char buff[BUFF_SZ] = {0};
    connfd = accept(sockfd, (SA*) & cli, &len);
    
    driver->connection_fd = connfd;

    if (connfd < 0) {
        D("Err: server acccept failed modem..., err=%s\n", strerror(errno));
        exit(-1);
    }
    
    while (1) {
        size_t readed = read(connfd, buff, BUFF_SZ);
        modem_driver_read(driver, (unsigned char *)buff, readed, connfd);
        memset(buff, 0, BUFF_SZ);
    }
}

static void control_console() {
    int sockfd, connfd;
    pthread_t threadId;
    struct sockaddr_in servaddr = {0}, cli = {0};
    ControlClient       client;
    socklen_t len = sizeof(struct sockaddr_in);
    
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1)
        error("Err: socket creation failed...");
#ifdef BUILD_HOST
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0)
        error("Err: setsockopt(SO_REUSEADDR) failed");
#else 
    int enable = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0)
        error("Err: setsockopt(SO_REUSEADDR) failed");
#endif

    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(COMMAND_PORT);

    if ((bind(sockfd, (SA*) & servaddr, sizeof (servaddr))) != 0)
        error("Err: failed to bind socket");

    if ((listen(sockfd, 8)) != 0)
        error("Err: listen failed");

    I("Telnet server listening...");
    for( ;; ) {
        connfd = accept(sockfd, (SA*) & cli, &len);
        ControlGlobal  global = &_g_global;
        memset( global, 0, sizeof(*global) );
        client = control_client_create( connfd, global );
        if (client) {
            const char *greeting = "Android Console: type 'help' for a list of commands\r\n" ;
            write(connfd, greeting, strlen(greeting));
            write( connfd, "OK\r\n", 4 );
        } else
            error("Err: cant create control client");

        if (connfd < 0)
            error("Err: server acccept failed console...");
        if(pthread_create(&threadId, NULL, (void*(*)(void*))control_client_read, client))
            error("Err: cant create console thread");
    }
}


int main(int argc, char** argv) {

    I("STARTING ANDROID VIRTUAL HARDWARE INTERFACE");
    (void)argv;
    (void)argc;
    pthread_t thread_id; int ret; 
    ModemDriver * driver = android_modem_init(PHONE_NUM);
    if(!driver)
        error("Err: could not init modem device");
    ret = pthread_create(&thread_id, NULL, modem_listener, driver);
    if(ret !=0)
        error("Err: could not create thread for handling modem commands");
    if(!init_gps_client()) {
        control_console();
        return (EXIT_SUCCESS);
    } else
        return (EXIT_FAILURE);
}

