
#ifndef CONSOLE_H
#define CONSOLE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ControlGlobalRec_*  ControlGlobal;

typedef struct ControlClientRec_*  ControlClient;

typedef struct {
    int           host_port;
    int           host_udp;
    unsigned int  guest_ip;
    int           guest_port;
} RedirRec, *Redir;

typedef int Socket;

typedef struct ControlClientRec_
{
    struct ControlClientRec_*  next;       /* next client in list           */
    Socket                     sock;       /* socket used for communication */
    ControlGlobal              global;
    char                       finished;
    char                       buff[ 4096 ];
    int                        buff_len;

} ControlClientRec;


typedef struct ControlGlobalRec_
{
    /* listening socket */
    Socket    listen_fd;

    /* the list of current clients */
    ControlClient   clients;

    /* the list of redirections currently active */
    Redir     redirs;
    int       num_redirs;
    int       max_redirs;

} ControlGlobalRec;

extern ControlGlobalRec  _g_global;

ControlClient
control_client_create( Socket         socket,
                       ControlGlobal  global );
void control_client_read( void*  _client );
int do_sms_send( ControlClient  client, char*  args );

#ifdef __cplusplus
}
#endif

#endif /* CONSOLE_H */

