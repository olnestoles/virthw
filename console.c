/* Copyright (C) 2007-2008 The Android Open Source Project
**
** This software is licensed under the terms of the GNU General Public
** License version 2, as published by the Free Software Foundation, and
** may be copied, distributed, and modified under those terms.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/
/*
 *  Android emulator control console
 *
 *  this console is enabled automatically at emulator startup, on port 5554 by default,
 *  unless some other emulator is already running. See (android_emulation_start in android_sdl.c
 *  for details)
 *
 *  you can telnet to the console, then use commands like 'help' or others to dynamically
 *  change emulator settings.
 *
 */

#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "sms.h"
#include "gsm.h"
#include "modem_driver.h"
#include "stralloc.h"
#include "gps_client.h"
#include "console.h"
#include "android_modem.h"

#if defined(CONFIG_SLIRP)
#include "libslirp.h"
#endif

#define SA struct sockaddr

extern ModemDriver *_g_modem_driver;
extern void android_emulator_set_window_scale(double, int);

#define  DEBUG  0

#if 1
#  define  D_ACTIVE   VERBOSE_CHECK(console)
#else
#  define  D_ACTIVE   DEBUG
#endif

#if DEBUG
#  define  D(x)   do { if (D_ACTIVE) ( printf x , fflush(stdout) ); } while (0)
#  define  I(x)   do { if (D_ACTIVE) ( printf x , fflush(stdout) ); } while (0)
#else
#  define  D(x)   do{}while(0)
#  define  I(x)   do{}while(0)
#endif


ControlGlobalRec  _g_global;

#ifdef CONFIG_STANDALONE_CORE
/* UI client currently attached to the core. */
ControlClient attached_ui_client = NULL;

/* User events service client. */
ControlClient user_events_client = NULL;

/* UI control service client (UI -> Core). */
ControlClient ui_core_ctl_client = NULL;

/* UI control service (UI -> Core. */
// CoreUICtl* ui_core_ctl = NULL;

/* UI control service client (Core-> UI). */
ControlClient core_ui_ctl_client = NULL;
#endif  // CONFIG_STANDALONE_CORE

//static int
//control_global_add_redir( ControlGlobal  global,
//                          int            host_port,
//                          int            host_udp,
//                          unsigned int   guest_ip,
//                          int            guest_port )
//{
//    Redir  redir;
//
//    if (global->num_redirs >= global->max_redirs)
//    {
//        int  old_max = global->max_redirs;
//        int  new_max = old_max + (old_max >> 1) + 4;
//
//        Redir  new_redirs = realloc( global->redirs, new_max*sizeof(global->redirs[0]) );
//        if (new_redirs == NULL)
//            return -1;
//
//        global->redirs     = new_redirs;
//        global->max_redirs = new_max;
//    }
//
//    redir = &global->redirs[ global->num_redirs++ ];
//
//    redir->host_port  = host_port;
//    redir->host_udp   = host_udp;
//    redir->guest_ip   = guest_ip;
//    redir->guest_port = guest_port;
//
//    return 0;
//}

//static int
//control_global_del_redir( ControlGlobal  global,
//                          int            host_port,
//                          int            host_udp )
//{
//    int  nn;
//
//    for (nn = 0; nn < global->num_redirs; nn++)
//    {
//        Redir  redir = &global->redirs[nn];
//
//        if ( redir->host_port == host_port &&
//             redir->host_udp  == host_udp  )
//        {
//            memmove( redir, redir + 1, ((global->num_redirs - nn)-1)*sizeof(*redir) );
//            global->num_redirs -= 1;
//            return 0;
//        }
//    }
//    /* we didn't find it */
//    return -1;
//}

/* Detach the socket descriptor from a given ControlClient
 * and return its value. This is useful either when destroying
 * the client, or redirecting the socket to another service.
 *
 * NOTE: this does not close the socket.
 */
static int
control_client_detach( ControlClient  client )
{
    int  result;

    if (client->sock < 0)
        return -1;

    result = client->sock;
    client->sock = -1;

    return result;
}

static void
control_client_destroy( ControlClient  client )
{
    ControlGlobal  global = client->global;
    ControlClient  *pnode = &global->clients;
    int            sock;

    D(( "destroying control client %p\n", client ));

#ifdef CONFIG_STANDALONE_CORE
    if (client == attached_ui_client) {
        attachUiProxy_destroy();
        attached_ui_client = NULL;
    }

    if (client == user_events_client) {
        userEventsImpl_destroy();
        user_events_client = NULL;
    }

    if (client == ui_core_ctl_client) {
        coreCmdImpl_destroy();
        ui_core_ctl_client = NULL;
    }

    if (client == core_ui_ctl_client) {
        uiCmdProxy_destroy();
        core_ui_ctl_client = NULL;
    }
#endif  // CONFIG_STANDALONE_CORE

    sock = control_client_detach( client );
    if (sock >= 0)
        close(sock);

    for ( ;; ) {
        ControlClient  node = *pnode;
        if ( node == NULL )
            break;
        if ( node == client ) {
            *pnode     = node->next;
            node->next = NULL;
            break;
        }
        pnode = &node->next;
    }

    free( client );
}



static void  control_control_write( ControlClient  client, const char*  buff, int  len )
{
    int ret;

    if (len < 0)
        len = strlen(buff);

    while (len > 0) {
        ret = write( client->sock, buff, len);
        if (ret < 0) {
            if (errno != EWOULDBLOCK && errno != EAGAIN)
                return;
        } else {
            buff += ret;
            len  -= ret;
        }
    }
}

static int  control_vwrite( ControlClient  client, const char*  format, va_list args )
{
    static char  temp[1024];
    int ret = vsnprintf( temp, sizeof(temp), format, args );
    temp[ sizeof(temp)-1 ] = 0;
    control_control_write( client, temp, -1 );

    return ret;
}

static int  control_write( ControlClient  client, const char*  format, ... )
{
    int ret;
    va_list      args;
    va_start(args, format);
    ret = control_vwrite(client, format, args);
    va_end(args);

    return ret;
}


ControlClient
control_client_create( Socket         socket,
                       ControlGlobal  global )
{
    ControlClient  client = calloc( sizeof(*client), 1 );

    if (client) {
        client->finished = 0;
        client->global  = global;
        client->sock    = socket;
        client->next    = global->clients;
        global->clients = client;
    }
    return client;
}

typedef const struct CommandDefRec_  *CommandDef;

typedef struct CommandDefRec_ {
    const char*  names;
    const char*  abstract;
    const char*  description;
    void        (*descriptor)( ControlClient  client );
    int         (*handler)( ControlClient  client, char* args );
    CommandDef   subcommands;   /* if handler is NULL */

} CommandDefRec;

static const CommandDefRec   main_commands[];  /* forward */

static CommandDef
find_command( char*  input, CommandDef  commands, char*  *pend, char*  *pargs )
{
    int    nn;
    char*  args = strchr(input, ' ');

    if (args != NULL) {
        while (*args == ' ')
            args++;

        if (args[0] == 0)
            args = NULL;
    }

    for (nn = 0; commands[nn].names != NULL; nn++)
    {
        const char*  name = commands[nn].names;
        const char*  sep;

        do {
            int  len, c;

            sep = strchr( name, '|' );
            if (sep)
                len = sep - name;
            else
                len = strlen(name);

            c = input[len];
            if ( !memcmp( name, input, len ) && (c == ' ' || c == 0) ) {
                *pend  = input + len;
                *pargs = args;
                return &commands[nn];
            }

            if (sep)
                name = sep + 1;

        } while (sep != NULL && *name);
    }
    /* NOTE: don't touch *pend and *pargs if no command is found */
    return NULL;
}

static void
dump_help( ControlClient  client,
           CommandDef     cmd,
           const char*    prefix )
{
    if (cmd->description) {
        control_write( client, "%s", cmd->description );
    } else if (cmd->descriptor) {
        cmd->descriptor( client );
    } else
        control_write( client, "%s\r\n", cmd->abstract );

    if (cmd->subcommands) {
        cmd = cmd->subcommands;
        control_write( client, "\r\navailable sub-commands:\r\n" );
        for ( ; cmd->names != NULL; cmd++ ) {
            control_write( client, "   %s %-15s  %s\r\n", prefix, cmd->names, cmd->abstract );
        }
        control_write( client, "\r\n" );
    }
}

static void
control_client_do_command( ControlClient  client )
{
    char*       line     = client->buff;
    char*       args     = NULL;
    CommandDef  commands = main_commands;
    char*       cmdend   = client->buff;
    CommandDef  cmd      = find_command( line, commands, &cmdend, &args );

    if (cmd == NULL) {
        control_write( client, "KO: unknown command, try 'help'\r\n" );
        return;
    }

    for (;;) {
        CommandDef  subcmd;

        if (cmd->handler) {
            if ( !cmd->handler( client, args ) ) {
                control_write( client, "OK\r\n" );
            }
            break;
        }

        /* no handler means we should have sub-commands */
        if (cmd->subcommands == NULL) {
            control_write( client, "KO: internal error: buggy command table for '%.*s'\r\n",
                           cmdend - client->buff, client->buff );
            break;
        }

        /* we need a sub-command here */
        if ( !args ) {
            dump_help( client, cmd, "" );
            control_write( client, "KO: missing sub-command\r\n" );
            break;
        }

        line     = args;
        commands = cmd->subcommands;
        subcmd   = find_command( line, commands, &cmdend, &args );
        if (subcmd == NULL) {
            dump_help( client, cmd, "" );
            control_write( client, "KO:  bad sub-command\r\n" );
            break;
        }
        cmd = subcmd;
    }
}

/* implement the 'help' command */
static int
do_help( ControlClient  client, char*  args )
{
    char*       line;
    char*       start = args;
    char*       end   = start;
    CommandDef  cmd = main_commands;

    /* without arguments, simply dump all commands */
    if (args == NULL) {
        control_write( client, "Android console command help:\r\n\r\n" );
        for ( ; cmd->names != NULL; cmd++ ) {
            control_write( client, "    %-15s  %s\r\n", cmd->names, cmd->abstract );
        }
        control_write( client, "\r\ntry 'help <command>' for command-specific help\r\n" );
        return 0;
    }

    /* with an argument, find the corresponding command */
    for (;;) {
        CommandDef  subcmd;

        line    = args;
        subcmd  = find_command( line, cmd, &end, &args );
        if (subcmd == NULL) {
            control_write( client, "try one of these instead:\r\n\r\n" );
            for ( ; cmd->names != NULL; cmd++ ) {
                control_write( client, "    %.*s %s\r\n",
                              end - start, start, cmd->names );
            }
            control_write( client, "\r\nKO: unknown command\r\n" );
            return -1;
        }

        if ( !args || !subcmd->subcommands ) {
            dump_help( client, subcmd, start );
            return 0;
        }
        cmd = subcmd->subcommands;
    }
}


static void
control_client_read_byte( ControlClient  client, unsigned char  ch )
{
    if (ch == '\r')
    {
        /* filter them out */
    }
    else if (ch == '\n')
    {
        client->buff[ client->buff_len ] = 0;
        control_client_do_command( client );
        if (client->finished)
            return;

        client->buff_len = 0;
    }
    else
    {
        if (client->buff_len >= sizeof(client->buff)-1)
            client->buff_len = 0;

        client->buff[ client->buff_len++ ] = ch;
    }
}

void
control_client_read( void*  _client )
{
    ControlClient  client = _client;
    unsigned char  buf[4096];
    int            size;

    D(( "in control_client read: " ));
    while(1) {
        size = read(client->sock, buf, sizeof(buf));
        if (size < 0) {
            perror("read socket\n");
            return;
        }

        if (size == 0) {
            /* end of connection */
            D(( "end of connection detected !!\n" ));
            control_client_destroy( client );
        }
        else {
            int  nn;
    #ifdef _WIN32
    #  if DEBUG
            char  temp[16];
            int   count = size > sizeof(temp)-1 ? sizeof(temp)-1 : size;
            for (nn = 0; nn < count; nn++) {
                    int  c = buf[nn];
                    if (c == '\n')
                            temp[nn] = '!';
                else if (c < 32)
                            temp[nn] = '.';
                    else
                        temp[nn] = (char)c;
            }
            temp[nn] = 0;
            D(( "received %d bytes: %s\n", size, temp ));
    #  endif
    #else
            D(( "received %.*s\n", size, buf ));
    #endif
            for (nn = 0; nn < size; nn++) {
                control_client_read_byte( client, buf[nn] );
                if (client->finished) {
                    control_client_destroy(client);
                    return;
                }
            }
        }
    }
}

static int
socket_setoption(int  fd, int  domain, int  option, int  _flag)
{
    int    flag = _flag;
    return setsockopt( fd, domain, option, (const char*)&flag, sizeof(flag) );
}

int socket_set_xreuseaddr(int  fd)
{
    return socket_setoption(fd, SOL_SOCKET, SO_REUSEADDR, 1);
}

/* this function is called on each new client connection */
static void
control_global_accept( void*  _global )
{
    ControlGlobal       global = _global;
    ControlClient       client;
    Socket              fd;

    D(( "control_global_accept: just in (fd=%d)\n", global->listen_fd ));
    unsigned int len;
    struct sockaddr_in cli;
    fd = accept(global->listen_fd, (SA*) & cli, &len);
    if (fd < 0) {
        D(( "problem in accept: %d: %s\n", errno, errno_str ));
        perror("accept");
        return;
    }

    socket_set_xreuseaddr( fd );

    D(( "control_global_accept: creating new client\n" ));
    client = control_client_create( fd, global );
    if (client) {
        D(( "control_global_accept: new client %p\n", client ));
        control_write( client, "Android Console: type 'help' for a list of commands\r\n" );
        control_write( client, "OK\r\n" );
    }
}

static int
control_global_init( ControlGlobal  global,
                     int            control_port )
{
    Socket  fd;
    int     ret;
//    SockAddress  sockaddr;
    struct sockaddr_in servaddr = {};

    memset( global, 0, sizeof(*global) );
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0){
        puts("setsockopt(SO_REUSEADDR) failed");
        exit(1);
    }
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(5544);
    ret = bind(fd, (SA*) & servaddr, sizeof (servaddr));
    
    if (ret < 0) {
        perror("bind");
        close( fd );
        return -1;
    }
    ret = listen(fd, 8);
    if (ret < 0) {
        perror("listen");
        close( fd );
        return -1;
    }

    global->listen_fd = fd;
    control_global_accept(global);
    return 0;
}



static int
do_quit( ControlClient  client, char*  args )
{
    client->finished = 1;
    return -1;
}

/********************************************************************************************/
/********************************************************************************************/
/*****                                                                                 ******/
/*****                          C D M A   M O D E M                                    ******/
/*****                                                                                 ******/
/********************************************************************************************/
/********************************************************************************************/

static const struct {
    const char *            name;
    const char *            display;
//    ACdmaSubscriptionSource source;
} _cdma_subscription_sources[] = {
//    { "nv",            "Read subscription from non-volatile RAM", A_SUBSCRIPTION_NVRAM },
//    { "ruim",          "Read subscription from RUIM", A_SUBSCRIPTION_RUIM },
};

static void
dump_subscription_sources( ControlClient client )
{
    int i;
    for (i = 0;
         i < sizeof(_cdma_subscription_sources) / sizeof(_cdma_subscription_sources[0]);
         i++) {
        control_write( client, "    %s: %s\r\n",
                       _cdma_subscription_sources[i].name,
                       _cdma_subscription_sources[i].display );
    }
}

static void
describe_subscription_source( ControlClient client )
{
    control_write( client,
                   "'cdma ssource <ssource>' allows you to specify where to read the subscription from\r\n" );
    dump_subscription_sources( client );
}

static int
do_cdma_ssource( ControlClient  client, char*  args )
{
    int nn;
    if (!args) {
        control_write( client, "KO: missing argument, try 'cdma ssource <source>'\r\n" );
        return -1;
    }

    for (nn = 0; ; nn++) {
        const char*         name    = _cdma_subscription_sources[nn].name;
//        ACdmaSubscriptionSource ssource = _cdma_subscription_sources[nn].source;

        if (!name)
            break;

        if (!strcasecmp( args, name )) {
//            amodem_set_cdma_subscription_source( android_modem, ssource );
            return 0;
        }
    }
    control_write( client, "KO: Don't know source %s\r\n", args );
    return -1;
}

static int
do_cdma_prl_version( ControlClient client, char * args )
{
    int version = 0;
    char *endptr;

    if (!args) {
        control_write( client, "KO: missing argument, try 'cdma prl_version <version>'\r\n");
        return -1;
    }

    version = strtol(args, &endptr, 0);
    if (endptr != args) {
        amodem_set_cdma_prl_version( android_modem, version );
    }
    return 0;
}
/********************************************************************************************/
/********************************************************************************************/
/*****                                                                                 ******/
/*****                           G S M   M O D E M                                     ******/
/*****                                                                                 ******/
/********************************************************************************************/
/********************************************************************************************/

//static const struct {
//    const char*         name;
//    const char*         display;
//    ARegistrationState  state;
//} _gsm_states[] = {
//    { "unregistered",  "no network available", A_REGISTRATION_UNREGISTERED },
//    { "home",          "on local network, non-roaming", A_REGISTRATION_HOME },
//    { "roaming",       "on roaming network", A_REGISTRATION_ROAMING },
//    { "searching",     "searching networks", A_REGISTRATION_SEARCHING },
//    { "denied",        "emergency calls only", A_REGISTRATION_DENIED },
//    { "off",           "same as 'unregistered'", A_REGISTRATION_UNREGISTERED },
//    { "on",            "same as 'home'", A_REGISTRATION_HOME },
//    { NULL, NULL, A_REGISTRATION_UNREGISTERED }
//};
//
//static const char*
//gsm_state_to_string( ARegistrationState  state )
//{
//    int  nn;
//    for (nn = 0; _gsm_states[nn].name != NULL; nn++) {
//        if (state == _gsm_states[nn].state)
//            return _gsm_states[nn].name;
//    }
//    return "<unknown>";
//}
//
//static int
//do_gsm_status( ControlClient  client, char*  args )
//{
//    if (args) {
//        control_write( client, "KO: no argument required\r\n" );
//        return -1;
//    }
//    if (!android_modem) {
//        control_write( client, "KO: modem emulation not running\r\n" );
//        return -1;
//    }
//    control_write( client, "gsm voice state: %s\r\n",
//                   gsm_state_to_string(
//                       amodem_get_voice_registration(android_modem) ) );
//    control_write( client, "gsm data state:  %s\r\n",
//                   gsm_state_to_string(
//                       amodem_get_data_registration(android_modem) ) );
//    return 0;
//}
//
//
//static void
//help_gsm_data( ControlClient  client )
//{
//    int  nn;
//    control_write( client,
//            "the 'gsm data <state>' allows you to change the state of your GPRS connection\r\n"
//            "valid values for <state> are the following:\r\n\r\n" );
//    for (nn = 0; ; nn++) {
//        const char*         name    = _gsm_states[nn].name;
//        const char*         display = _gsm_states[nn].display;
//
//        if (!name)
//            break;
//
//        control_write( client, "  %-15s %s\r\n", name, display );
//    }
//    control_write( client, "\r\n" );
//}
//
//
//static int
//do_gsm_data( ControlClient  client, char*  args )
//{
//    int  nn;
//
//    if (!args) {
//        control_write( client, "KO: missing argument, try 'gsm data <state>'\r\n" );
//        return -1;
//    }
//
//    for (nn = 0; ; nn++) {
//        const char*         name    = _gsm_states[nn].name;
//        ARegistrationState  state   = _gsm_states[nn].state;
//
//        if (!name)
//            break;
//
//        if ( !strcmp( args, name ) ) {
//            if (!android_modem) {
//                control_write( client, "KO: modem emulation not running\r\n" );
//                return -1;
//            }
//            amodem_set_data_registration( android_modem, state );
//            qemu_net_disable = (state != A_REGISTRATION_HOME    &&
//                                state != A_REGISTRATION_ROAMING );
//            return 0;
//        }
//    }
//    control_write( client, "KO: bad GSM data state name, try 'help gsm data' for list of valid values\r\n" );
//    return -1;
//}
//
//static void
//help_gsm_voice( ControlClient  client )
//{
//    int  nn;
//    control_write( client,
//            "the 'gsm voice <state>' allows you to change the state of your GPRS connection\r\n"
//            "valid values for <state> are the following:\r\n\r\n" );
//    for (nn = 0; ; nn++) {
//        const char*         name    = _gsm_states[nn].name;
//        const char*         display = _gsm_states[nn].display;
//
//        if (!name)
//            break;
//
//        control_write( client, "  %-15s %s\r\n", name, display );
//    }
//    control_write( client, "\r\n" );
//}
//
//
//static int
//do_gsm_voice( ControlClient  client, char*  args )
//{
//    int  nn;
//
//    if (!args) {
//        control_write( client, "KO: missing argument, try 'gsm voice <state>'\r\n" );
//        return -1;
//    }
//
//    for (nn = 0; ; nn++) {
//        const char*         name    = _gsm_states[nn].name;
//        ARegistrationState  state   = _gsm_states[nn].state;
//
//        if (!name)
//            break;
//
//        if ( !strcmp( args, name ) ) {
//            if (!android_modem) {
//                control_write( client, "KO: modem emulation not running\r\n" );
//                return -1;
//            }
//            amodem_set_voice_registration( android_modem, state );
//            return 0;
//        }
//    }
//    control_write( client, "KO: bad GSM data state name, try 'help gsm voice' for list of valid values\r\n" );
//    return -1;
//}
//
//
static int
gsm_check_number( char*  args )
{
    int  nn;

    for (nn = 0; args[nn] != 0; nn++) {
        int  c = args[nn];
        if ( !isdigit(c) && c != '+' && c != '#' ) {
            return -1;
        }
    }
    if (nn == 0)
        return -1;

    return 0;
}
//
static int
do_gsm_call( ControlClient  client, char*  args )
{
    /* check that we have a phone number made of digits */
    if (!args) {
        control_write( client, "KO: missing argument, try 'gsm call <phonenumber>'\r\n" );
        return -1;
    }

    if (gsm_check_number(args)) {
        control_write( client, "KO: bad phone number format, use digits, # and + only\r\n" );
        return -1;
    }

    if (!android_modem) {
        control_write( client, "KO: modem emulation not running\r\n" );
        return -1;
    }
    amodem_add_inbound_call( android_modem, args );
    return 0;
}

static int
do_ussd_reply( ControlClient  client, char*  args )
{
#define MAX_USSD_REPLY 4096
    char AT[] = "+CUSD: 0,";
    if( !args )
        return -1;
    size_t len = strlen (args);
    
    if(len + sizeof(AT) > MAX_USSD_REPLY)
        return -1;
    
    
    char *reply = malloc(MAX_USSD_REPLY);
    if( reply == 0 )
        return -1;
    
    snprintf(reply,MAX_USSD_REPLY, "%s%s", AT, args);
    
    ussd_reply(reply);
    free(reply);
    return 0;
}

//
//static int
//do_gsm_cancel( ControlClient  client, char*  args )
//{
//    if (!args) {
//        control_write( client, "KO: missing argument, try 'gsm call <phonenumber>'\r\n" );
//        return -1;
//    }
//    if (gsm_check_number(args)) {
//        control_write( client, "KO: bad phone number format, use digits, # and + only\r\n" );
//        return -1;
//    }
//    if (!android_modem) {
//        control_write( client, "KO: modem emulation not running\r\n" );
//        return -1;
//    }
//    if ( amodem_disconnect_call( android_modem, args ) < 0 ) {
//        control_write( client, "KO: could not cancel this number\r\n" );
//        return -1;
//    }
//    return 0;
//}
//
//
//static const char*
//call_state_to_string( ACallState  state )
//{
//    switch (state) {
//        case A_CALL_ACTIVE:   return "active";
//        case A_CALL_HELD:     return "held";
//        case A_CALL_ALERTING: return "ringing";
//        case A_CALL_WAITING:  return "waiting";
//        case A_CALL_INCOMING: return "incoming";
//        default: return "unknown";
//    }
//}
//
//static int
//do_gsm_list( ControlClient  client, char*  args )
//{
//    /* check that we have a phone number made of digits */
//    int   count = amodem_get_call_count( android_modem );
//    int   nn;
//    for (nn = 0; nn < count; nn++) {
//        ACall        call = amodem_get_call( android_modem, nn );
//        const char*  dir;
//
//        if (call == NULL)
//            continue;
//
//        if (call->dir == A_CALL_OUTBOUND)
//            dir = "outbound to ";
//         else
//            dir = "inbound from";
//
//        control_write( client, "%s %-10s : %s\r\n", dir,
//                       call->number, call_state_to_string(call->state) );
//    }
//    return 0;
//}
//
//static int
//do_gsm_busy( ControlClient  client, char*  args )
//{
//    ACall  call;
//
//    if (!args) {
//        control_write( client, "KO: missing argument, try 'gsm busy <phonenumber>'\r\n" );
//        return -1;
//    }
//    call = amodem_find_call_by_number( android_modem, args );
//    if (call == NULL || call->dir != A_CALL_OUTBOUND) {
//        control_write( client, "KO: no current outbound call to number '%s' (call %p)\r\n", args, call );
//        return -1;
//    }
//    if ( amodem_disconnect_call( android_modem, args ) < 0 ) {
//        control_write( client, "KO: could not cancel this number\r\n" );
//        return -1;
//    }
//    return 0;
//}
//
//static int
//do_gsm_hold( ControlClient  client, char*  args )
//{
//    ACall  call;
//
//    if (!args) {
//        control_write( client, "KO: missing argument, try 'gsm out hold <phonenumber>'\r\n" );
//        return -1;
//    }
//    call = amodem_find_call_by_number( android_modem, args );
//    if (call == NULL) {
//        control_write( client, "KO: no current call to/from number '%s'\r\n", args );
//        return -1;
//    }
//    if ( amodem_update_call( android_modem, args, A_CALL_HELD ) < 0 ) {
//        control_write( client, "KO: could put this call on hold\r\n" );
//        return -1;
//    }
//    return 0;
//}
//
//
//static int
//do_gsm_accept( ControlClient  client, char*  args )
//{
//    ACall  call;
//
//    if (!args) {
//        control_write( client, "KO: missing argument, try 'gsm accept <phonenumber>'\r\n" );
//        return -1;
//    }
//    call = amodem_find_call_by_number( android_modem, args );
//    if (call == NULL) {
//        control_write( client, "KO: no current call to/from number '%s'\r\n", args );
//        return -1;
//    }
//    if ( amodem_update_call( android_modem, args, A_CALL_ACTIVE ) < 0 ) {
//        control_write( client, "KO: could not activate this call\r\n" );
//        return -1;
//    }
//    return 0;
//}
//
//static int
//do_gsm_signal( ControlClient  client, char*  args )
//{
//      enum { SIGNAL_RSSI = 0, SIGNAL_BER, NUM_SIGNAL_PARAMS };
//      char*   p = args;
//      int     top_param = -1;
//      int     params[ NUM_SIGNAL_PARAMS ];
//
//      static  int  last_ber = 99;
//
//      if (!p)
//          p = "";
//
//      /* tokenize */
//      while (*p) {
//          char*   end;
//          int  val = strtol( p, &end, 10 );
//
//          if (end == p) {
//              control_write( client, "KO: argument '%s' is not a number\n", p );
//              return -1;
//          }
//
//          params[++top_param] = val;
//          if (top_param + 1 == NUM_SIGNAL_PARAMS)
//              break;
//
//          p = end;
//          while (*p && (p[0] == ' ' || p[0] == '\t'))
//              p += 1;
//      }
//
//      /* sanity check */
//      if (top_param < SIGNAL_RSSI) {
//          control_write( client, "KO: not enough arguments: see 'help gsm signal' for details\r\n" );
//          return -1;
//      }
//
//      int rssi = params[SIGNAL_RSSI];
//      if ((rssi < 0 || rssi > 31) && rssi != 99) {
//          control_write( client, "KO: invalid RSSI - must be 0..31 or 99\r\n");
//          return -1;
//      }
//
//      /* check ber is 0..7 or 99 */
//      if (top_param >= SIGNAL_BER) {
//          int ber = params[SIGNAL_BER];
//          if ((ber < 0 || ber > 7) && ber != 99) {
//              control_write( client, "KO: invalid BER - must be 0..7 or 99\r\n");
//              return -1;
//          }
//          last_ber = ber;
//      }
//
//      amodem_set_signal_strength( android_modem, rssi, last_ber );
//
//      return 0;
//  }
//

#if 0
static const CommandDefRec  gsm_in_commands[] =
{
    { "new", "create a new 'waiting' inbound call",
    "'gsm in create <phonenumber>' creates a new inbound phone call, placed in\r\n"
    "the 'waiting' state by default, until the system answers/holds/closes it\r\n", NULL
    do_gsm_in_create, NULL },

    { "hold", "change the state of an oubtound call to 'held'",
    "change the state of an outbound call to 'held'. this is only possible\r\n"
    "if the call in the 'waiting' or 'active' state\r\n", NULL,
    do_gsm_out_hold, NULL },

    { "accept", "change the state of an outbound call to 'active'",
    "change the state of an outbound call to 'active'. this is only possible\r\n"
    "if the call is in the 'waiting' or 'held' state\r\n", NULL,
    do_gsm_out_accept, NULL },

    { NULL, NULL, NULL, NULL, NULL, NULL }
};
#endif


static const CommandDefRec  cdma_commands[] =
{
    { "ssource", "Set the current CDMA subscription source",
      NULL, describe_subscription_source,
      do_cdma_ssource, NULL },
    { "prl_version", "Dump the current PRL version",
      NULL, NULL,
      do_cdma_prl_version, NULL },
};

static const CommandDefRec  gsm_commands[] =
{
//    { "list", "list current phone calls",
//    "'gsm list' lists all inbound and outbound calls and their state\r\n", NULL,
//    do_gsm_list, NULL },

    { "call", "create inbound phone call",
    "'gsm call <phonenumber>' allows you to simulate a new inbound call\r\n", NULL,
    do_gsm_call, NULL },
    
    { "ussd", "create ussd service reply",
    "'gsm ussd <text>' allows you to simulate a ussd reply\r\n", NULL,
    do_ussd_reply, NULL },
            

//    { "busy", "close waiting outbound call as busy",
//    "'gsm busy <remoteNumber>' closes an outbound call, reporting\r\n"
//    "the remote phone as busy. only possible if the call is 'waiting'.\r\n", NULL,
//    do_gsm_busy, NULL },
//
//    { "hold", "change the state of an oubtound call to 'held'",
//    "'gsm hold <remoteNumber>' change the state of a call to 'held'. this is only possible\r\n"
//    "if the call in the 'waiting' or 'active' state\r\n", NULL,
//    do_gsm_hold, NULL },
//
//    { "accept", "change the state of an outbound call to 'active'",
//    "'gsm accept <remoteNumber>' change the state of a call to 'active'. this is only possible\r\n"
//    "if the call is in the 'waiting' or 'held' state\r\n", NULL,
//    do_gsm_accept, NULL },
//
//    { "cancel", "disconnect an inbound or outbound phone call",
//    "'gsm cancel <phonenumber>' allows you to simulate the end of an inbound or outbound call\r\n", NULL,
//    do_gsm_cancel, NULL },
//
//    { "data", "modify data connection state", NULL, help_gsm_data,
//    do_gsm_data, NULL },
//
//    { "voice", "modify voice connection state", NULL, help_gsm_voice,
//    do_gsm_voice, NULL },
//
//    { "status", "display GSM status",
//    "'gsm status' displays the current state of the GSM emulation\r\n", NULL,
//    do_gsm_status, NULL },
//
//    { "signal", "set sets the rssi and ber",
//    "'gsm signal <rssi> [<ber>]' changes the reported strength and error rate on next (15s) update.\r\n"
//    "rssi range is 0..31 and 99 for unknown\r\n"
//    "ber range is 0..7 percent and 99 for unknown\r\n",
//    NULL, do_gsm_signal, NULL },

    { NULL, NULL, NULL, NULL, NULL, NULL }
};

/********************************************************************************************/
/********************************************************************************************/
/*****                                                                                 ******/
/*****                           S M S   C O M M A N D                                 ******/
/*****                                                                                 ******/
/********************************************************************************************/
/********************************************************************************************/

int
do_sms_send( ControlClient  client, char*  args )
{
    char*          p;
    int            textlen;
    SmsAddressRec  sender;
    SmsPDU*        pdus;
    int            nn;

    /* check that we have a phone number made of digits */
    if (!args) {
    MissingArgument:
        control_write( client, "KO: missing argument, try 'sms send <phonenumber> <text message>'\r\n" );
        return -1;
    }
    p = strchr( args, ' ' );
    if (!p) {
        goto MissingArgument;
    }

    if ( sms_address_from_str( &sender, args, p - args ) < 0 ) {
        control_write( client, "KO: bad phone number format, must be [+](0-9)*\r\n" );
        return -1;
    }


    /* un-secape message text into proper utf-8 (conversion happens in-site) */
    p      += 1;
    textlen = strlen(p);
    textlen = sms_utf8_from_message_str( p, textlen, (unsigned char*)p, textlen );
    if (textlen < 0) {
        control_write( client, "message must be utf8 and can use the following escapes:\r\n"
                       "    \\n      for a newline\r\n"
                       "    \\xNN    where NN are two hexadecimal numbers\r\n"
                       "    \\uNNNN  where NNNN are four hexadecimal numbers\r\n"
                       "    \\\\     to send a '\\' character\r\n\r\n"
                       "    anything else is an error\r\n"
                       "KO: badly formatted text\r\n" );
        return -1;
    }

    if (!android_modem) {
        control_write( client, "KO: modem emulation not running\r\n" );
        return -1;
    }

    /* create a list of SMS PDUs, then send them */
    pdus = smspdu_create_deliver_utf8( (cbytes_t)p, textlen, &sender, NULL );
    if (pdus == NULL) {
        control_write( client, "KO: internal error when creating SMS-DELIVER PDUs\n" );
        return -1;
    }

    for (nn = 0; pdus[nn] != NULL; nn++)
        amodem_receive_sms( android_modem, pdus[nn] );

    smspdu_free_list( pdus );
    return 0;
}

static int
do_sms_sendpdu( ControlClient  client, char*  args )
{
//    SmsPDU  pdu;

    /* check that we have a phone number made of digits */
    if (!args) {
        control_write( client, "KO: missing argument, try 'sms sendpdu <hexstring>'\r\n" );
        return -1;
    }

//    if (!android_modem) {
//        control_write( client, "KO: modem emulation not running\r\n" );
//        return -1;
//    }

//    pdu = smspdu_create_from_hex( args, strlen(args) );
//    if (pdu == NULL) {
//        control_write( client, "KO: badly formatted <hexstring>\r\n" );
//        return -1;
//    }

//    amodem_receive_sms( android_modem, pdu );
//    smspdu_free( pdu );
    return 0;
}

static const CommandDefRec  sms_commands[] =
{
    { "send", "send inbound SMS text message",
    "'sms send <phonenumber> <message>' allows you to simulate a new inbound sms message\r\n", NULL,
    do_sms_send, NULL },

    { "pdu", "send inbound SMS PDU",
    "'sms pdu <hexstring>' allows you to simulate a new inbound sms PDU\r\n"
    "(used internally when one emulator sends SMS messages to another instance).\r\n"
    "you probably don't want to play with this at all\r\n", NULL,
    do_sms_sendpdu, NULL },

    { NULL, NULL, NULL, NULL, NULL, NULL }
};

#if 0
static void
do_control_write(void* data, const char* string)
{
    control_write((ControlClient)data, string);
}

static int
do_power_display( ControlClient client, char*  args )
{
    goldfish_battery_display(do_control_write, client);
    return 0;
}

static int
do_ac_state( ControlClient  client, char*  args )
{
    if (args) {
        if (strcasecmp(args, "on") == 0) {
            goldfish_battery_set_prop(1, POWER_SUPPLY_PROP_ONLINE, 1);
            return 0;
        }
        if (strcasecmp(args, "off") == 0) {
            goldfish_battery_set_prop(1, POWER_SUPPLY_PROP_ONLINE, 0);
            return 0;
        }
    }

    control_write( client, "KO: Usage: \"ac on\" or \"ac off\"\n" );
    return -1;
}

static int
do_battery_status( ControlClient  client, char*  args )
{
    if (args) {
        if (strcasecmp(args, "unknown") == 0) {
            goldfish_battery_set_prop(0, POWER_SUPPLY_PROP_STATUS, POWER_SUPPLY_STATUS_UNKNOWN);
            return 0;
        }
        if (strcasecmp(args, "charging") == 0) {
            goldfish_battery_set_prop(0, POWER_SUPPLY_PROP_STATUS, POWER_SUPPLY_STATUS_CHARGING);
            return 0;
        }
        if (strcasecmp(args, "discharging") == 0) {
            goldfish_battery_set_prop(0, POWER_SUPPLY_PROP_STATUS, POWER_SUPPLY_STATUS_DISCHARGING);
            return 0;
        }
        if (strcasecmp(args, "not-charging") == 0) {
            goldfish_battery_set_prop(0, POWER_SUPPLY_PROP_STATUS, POWER_SUPPLY_STATUS_NOT_CHARGING);
            return 0;
        }
        if (strcasecmp(args, "full") == 0) {
            goldfish_battery_set_prop(0, POWER_SUPPLY_PROP_STATUS, POWER_SUPPLY_STATUS_FULL);
            return 0;
        }
    }

    control_write( client, "KO: Usage: \"status unknown|charging|discharging|not-charging|full\"\n" );
    return -1;
}

static int
do_battery_present( ControlClient  client, char*  args )
{
    if (args) {
        if (strcasecmp(args, "true") == 0) {
            goldfish_battery_set_prop(0, POWER_SUPPLY_PROP_PRESENT, 1);
            return 0;
        }
        if (strcasecmp(args, "false") == 0) {
            goldfish_battery_set_prop(0, POWER_SUPPLY_PROP_PRESENT, 0);
            return 0;
        }
    }

    control_write( client, "KO: Usage: \"present true\" or \"present false\"\n" );
    return -1;
}

static int
do_battery_health( ControlClient  client, char*  args )
{
    if (args) {
        if (strcasecmp(args, "unknown") == 0) {
            goldfish_battery_set_prop(0, POWER_SUPPLY_PROP_HEALTH, POWER_SUPPLY_HEALTH_UNKNOWN);
            return 0;
        }
        if (strcasecmp(args, "good") == 0) {
            goldfish_battery_set_prop(0, POWER_SUPPLY_PROP_HEALTH, POWER_SUPPLY_HEALTH_GOOD);
            return 0;
        }
        if (strcasecmp(args, "overheat") == 0) {
            goldfish_battery_set_prop(0, POWER_SUPPLY_PROP_HEALTH, POWER_SUPPLY_HEALTH_OVERHEAT);
            return 0;
        }
        if (strcasecmp(args, "dead") == 0) {
            goldfish_battery_set_prop(0, POWER_SUPPLY_PROP_HEALTH, POWER_SUPPLY_HEALTH_DEAD);
            return 0;
        }
        if (strcasecmp(args, "overvoltage") == 0) {
            goldfish_battery_set_prop(0, POWER_SUPPLY_PROP_HEALTH, POWER_SUPPLY_HEALTH_OVERVOLTAGE);
            return 0;
        }
        if (strcasecmp(args, "failure") == 0) {
            goldfish_battery_set_prop(0, POWER_SUPPLY_PROP_HEALTH, POWER_SUPPLY_HEALTH_UNSPEC_FAILURE);
            return 0;
        }
    }

    control_write( client, "KO: Usage: \"health unknown|good|overheat|dead|overvoltage|failure\"\n" );
    return -1;
}

static int
do_battery_capacity( ControlClient  client, char*  args )
{
    if (args) {
        int capacity;

        if (sscanf(args, "%d", &capacity) == 1 && capacity >= 0 && capacity <= 100) {
            goldfish_battery_set_prop(0, POWER_SUPPLY_PROP_CAPACITY, capacity);
            return 0;
        }
    }

    control_write( client, "KO: Usage: \"capacity <percentage>\"\n" );
    return -1;
}


static const CommandDefRec  power_commands[] =
{
    { "display", "display battery and charger state",
    "display battery and charger state\r\n", NULL,
    do_power_display, NULL },

    { "ac", "set AC charging state",
    "'ac on|off' allows you to set the AC charging state to on or off\r\n", NULL,
    do_ac_state, NULL },

    { "status", "set battery status",
    "'status unknown|charging|discharging|not-charging|full' allows you to set battery status\r\n", NULL,
    do_battery_status, NULL },

    { "present", "set battery present state",
    "'present true|false' allows you to set battery present state to true or false\r\n", NULL,
    do_battery_present, NULL },

    { "health", "set battery health state",
    "'health unknown|good|overheat|dead|overvoltage|failure' allows you to set battery health state\r\n", NULL,
    do_battery_health, NULL },

    { "capacity", "set battery capacity state",
    "'capacity <percentage>' allows you to set battery capacity to a value 0 - 100\r\n", NULL,
    do_battery_capacity, NULL },

    { NULL, NULL, NULL, NULL, NULL, NULL }
};

#endif

/********************************************************************************************/
/********************************************************************************************/
/*****                                                                                 ******/
/*****                             G E O   C O M M A N D S                             ******/
/*****                                                                                 ******/
/********************************************************************************************/
/********************************************************************************************/

static int
do_geo_nmea( ControlClient  client, char*  args )
{
    if (!args) {
        control_write( client, "KO: NMEA sentence missing, try 'help geo nmea'\r\n" );
        return -1;
    }
//    if (!android_gps_cs) {
//        control_write( client, "KO: no GPS emulation in this virtual device\r\n" );
//        return -1;
//    }
//    android_gps_send_nmea( args );
    return 0;
}

static int
do_geo_fix( ControlClient  client, char*  args )
{
    // GEO_SAT2 provides bug backwards compatibility.
    enum { GEO_LONG = 0, GEO_LAT, GEO_ALT, GEO_SAT, GEO_SAT2, NUM_GEO_PARAMS };
    char*   p = args;
    int     top_param = -1;
    double  params[ NUM_GEO_PARAMS ];
    int     n_satellites = 1;

    static  int last_time = 0;

    if (!p)
        p = "";

    /* tokenize */
    while (*p) {
        char*   end;
        double  val = strtod( p, &end );

        if (end == p) {
            control_write( client, "KO: argument '%s' is not a number\n", p );
            return -1;
        }

        params[++top_param] = val;
        if (top_param + 1 == NUM_GEO_PARAMS)
            break;

        p = end;
        while (*p && (p[0] == ' ' || p[0] == '\t'))
            p += 1;
    }

    /* sanity check */
    if (top_param < GEO_LAT) {
        control_write( client, "KO: not enough arguments: see 'help geo fix' for details\r\n" );
        return -1;
    }

    /* check number of satellites, must be integer between 1 and 12 */
    if (top_param >= GEO_SAT) {
        int sat_index = (top_param >= GEO_SAT2) ? GEO_SAT2 : GEO_SAT;
        n_satellites = (int) params[sat_index];
        if (n_satellites != params[sat_index]
            || n_satellites < 1 || n_satellites > 12) {
            control_write( client, "KO: invalid number of satellites. Must be an integer between 1 and 12\r\n");
            return -1;
        }
    }

    /* generate an NMEA sentence for this fix */
    {
        STRALLOC_DEFINE(s);
        double   val;
        int      deg, min;
        char     hemi;

        /* format overview:
         *    time of fix      123519     12:35:19 UTC
         *    latitude         4807.038   48 degrees, 07.038 minutes
         *    north/south      N or S
         *    longitude        01131.000  11 degrees, 31. minutes
         *    east/west        E or W
         *    fix quality      1          standard GPS fix
         *    satellites       1 to 12    number of satellites being tracked
         *    HDOP             <dontcare> horizontal dilution
         *    altitude         546.       altitude above sea-level
         *    altitude units   M          to indicate meters
         *    diff             <dontcare> height of sea-level above ellipsoid
         *    diff units       M          to indicate meters (should be <dontcare>)
         *    dgps age         <dontcare> time in seconds since last DGPS fix
         *    dgps sid         <dontcare> DGPS station id
         */

        /* first, the time */
        stralloc_add_format( s, "$GPGGA,%06d", last_time );
        last_time ++;

        /* then the latitude */
        hemi = 'N';
        val  = params[GEO_LAT];
        if (val < 0) {
            hemi = 'S';
            val  = -val;
        }
        deg = (int) val;
        val = 60*(val - deg);
        min = (int) val;
        val = 10000*(val - min);
        stralloc_add_format( s, ",%02d%02d.%04d,%c", deg, min, (int)val, hemi );

        /* the longitude */
        hemi = 'E';
        val  = params[GEO_LONG];
        if (val < 0) {
            hemi = 'W';
            val  = -val;
        }
        deg = (int) val;
        val = 60*(val - deg);
        min = (int) val;
        val = 10000*(val - min);
        stralloc_add_format( s, ",%02d%02d.%04d,%c", deg, min, (int)val, hemi );

        /* bogus fix quality, satellite count and dilution */
        stralloc_add_format( s, ",1,%02d,", n_satellites );

        /* optional altitude + bogus diff */
        if (top_param >= GEO_ALT) {
            stralloc_add_format( s, ",%.1g,M,0.,M", params[GEO_ALT] );
        } else {
            stralloc_add_str( s, ",,,," );
        }
        /* bogus rest and checksum */
        stralloc_add_str( s, ",,,*47" );

        /* send it, then free */
//        android_gps_send_nmea( stralloc_cstr(s) );
        char *str = stralloc_cstr(s);
        //D("geo fix=%s\n", str);
        
        update_gps_hw(str);
        
        stralloc_reset( s );
    }
    return 0;
}

static const CommandDefRec  geo_commands[] =
{
    { "nmea", "send an GPS NMEA sentence",
    "'geo nema <sentence>' sends a NMEA 0183 sentence to the emulated device, as\r\n"
    "if it came from an emulated GPS modem. <sentence> must begin with '$GP'. only\r\n"
    "'$GPGGA' and '$GPRCM' sentences are supported at the moment.\r\n",
    NULL, do_geo_nmea, NULL },

    { "fix", "send a simple GPS fix",
    "'geo fix <longitude> <latitude> [<altitude> [<satellites>]]'\r\n"
    " allows you to send a simple GPS fix to the emulated system.\r\n"
    " The parameters are:\r\n\r\n"
    "  <longitude>   longitude, in decimal degrees\r\n"
    "  <latitude>    latitude, in decimal degrees\r\n"
    "  <altitude>    optional altitude in meters\r\n"
    "  <satellites>  number of satellites being tracked (1-12)\r\n"
    "\r\n",
    NULL, do_geo_fix, NULL },

    { NULL, NULL, NULL, NULL, NULL, NULL }
};


#if 0
/********************************************************************************************/
/********************************************************************************************/
/*****                                                                                 ******/
/*****                        S E N S O R S  C O M M A N D S                           ******/
/*****                                                                                 ******/
/********************************************************************************************/
/********************************************************************************************/

/* For sensors user prompt string size.*/
#define SENSORS_INFO_SIZE 150

/* Get sensor data - (a,b,c) from sensor name */
static int
do_sensors_get( ControlClient client, char* args )
{
    if (! args) {
        control_write( client, "KO: Usage: \"get <sensorname>\"\n" );
        return -1;
    }

    int status = SENSOR_STATUS_UNKNOWN;
    char sensor[strlen(args) + 1];
    if (1 != sscanf( args, "%s", &sensor[0] ))
        goto SENSOR_STATUS_ERROR;

    int sensor_id = android_sensors_get_id_from_name( sensor );
    char buffer[SENSORS_INFO_SIZE] = { 0 };
    float a, b, c;

    if (sensor_id < 0) {
        status = sensor_id;
        goto SENSOR_STATUS_ERROR;
    } else {
        status = android_sensors_get( sensor_id, &a, &b, &c );
        if (status != SENSOR_STATUS_OK)
            goto SENSOR_STATUS_ERROR;
        snprintf( buffer, sizeof(buffer),
                "%s = %g:%g:%g\r\n", sensor, a, b, c );
        do_control_write( client, buffer );
        return 0;
    }

SENSOR_STATUS_ERROR:
    switch(status) {
    case SENSOR_STATUS_NO_SERVICE:
        snprintf( buffer, sizeof(buffer), "KO: No sensor service found!\r\n" );
        break;
    case SENSOR_STATUS_DISABLED:
        snprintf( buffer, sizeof(buffer), "KO: '%s' sensor is disabled.\r\n", sensor );
        break;
    case SENSOR_STATUS_UNKNOWN:
        snprintf( buffer, sizeof(buffer),
                "KO: unknown sensor name: %s, run 'sensor status' to get available sensors.\r\n", sensor );
        break;
    default:
        snprintf( buffer, sizeof(buffer), "KO: '%s' sensor: exception happens.\r\n", sensor );
    }
    do_control_write( client, buffer );
    return -1;
}

/* set sensor data - (a,b,c) from sensor name */
static int
do_sensors_set( ControlClient client, char* args )
{
    if (! args) {
        control_write( client, "KO: Usage: \"set <sensorname> <value-a>[:<value-b>[:<value-c>]]\"\n" );
        return -1;
    }

    int status;
    char* sensor;
    char* value;
    char* args_dup = strdup( args );
    if (args_dup == NULL) {
        control_write( client, "KO: Memory allocation failed.\n" );
        return -1;
    }
    char* p = args_dup;

    /* Parsing the args to get sensor name string */
    while (*p && isspace(*p)) p++;
    if (*p == 0)
        goto INPUT_ERROR;
    sensor = p;

    /* Parsing the args to get value string */
    while (*p && (! isspace(*p))) p++;
    if (*p == 0 || *(p + 1) == 0/* make sure value isn't NULL */)
        goto INPUT_ERROR;
    *p = 0;
    value = p + 1;

    if (! (strlen(sensor) && strlen(value)))
        goto INPUT_ERROR;

    int sensor_id = android_sensors_get_id_from_name( sensor );
    char buffer[SENSORS_INFO_SIZE] = { 0 };

    if (sensor_id < 0) {
        status = sensor_id;
        goto SENSOR_STATUS_ERROR;
    } else {
        float fvalues[3];
        status = android_sensors_get( sensor_id, &fvalues[0], &fvalues[1], &fvalues[2] );
        if (status != SENSOR_STATUS_OK)
            goto SENSOR_STATUS_ERROR;

        /* Parsing the value part to get the sensor values(a, b, c) */
        int i;
        char* pnext;
        char* pend = value + strlen(value);
        for (i = 0; i < 3; i++, value = pnext + 1) {
            pnext=strchr( value, ':' );
            if (pnext) {
                *pnext = 0;
            } else {
                pnext = pend;
            }

            if (pnext > value) {
                if (1 != sscanf( value,"%g", &fvalues[i] ))
                    goto INPUT_ERROR;
            }
        }

        status = android_sensors_set( sensor_id, fvalues[0], fvalues[1], fvalues[2] );
        if (status != SENSOR_STATUS_OK)
            goto SENSOR_STATUS_ERROR;

        free( args_dup );
        return 0;
    }

SENSOR_STATUS_ERROR:
    switch(status) {
    case SENSOR_STATUS_NO_SERVICE:
        snprintf( buffer, sizeof(buffer), "KO: No sensor service found!\r\n" );
        break;
    case SENSOR_STATUS_DISABLED:
        snprintf( buffer, sizeof(buffer), "KO: '%s' sensor is disabled.\r\n", sensor );
        break;
    case SENSOR_STATUS_UNKNOWN:
        snprintf( buffer, sizeof(buffer),
                "KO: unknown sensor name: %s, run 'sensor status' to get available sensors.\r\n", sensor );
        break;
    default:
        snprintf( buffer, sizeof(buffer), "KO: '%s' sensor: exception happens.\r\n", sensor );
    }
    do_control_write( client, buffer );
    free( args_dup );
    return -1;

INPUT_ERROR:
    control_write( client, "KO: Usage: \"set <sensorname> <value-a>[:<value-b>[:<value-c>]]\"\n" );
    free( args_dup );
    return -1;
}

/* get all available sensor names and enable status respectively. */
static int
do_sensors_status( ControlClient client, char* args )
{
    uint8_t id, status;
    char buffer[SENSORS_INFO_SIZE] = { 0 };

    for(id = 0; id < MAX_SENSORS; id++) {
        status = android_sensors_get_sensor_status( id );
        snprintf( buffer, sizeof(buffer), "%s: %s\n",
                android_sensors_get_name_from_id(id), (status ? "enabled.":"disabled.") );
        control_write( client, buffer );
    }

    return 0;
}

/* Sensor commands for get/set sensor values and get available sensor names. */
static const CommandDefRec sensor_commands[] =
{
    { "status", "list all sensors and their status.",
      "'status': list all sensors and their status.\r\n",
      NULL, do_sensors_status, NULL },

    { "get", "get sensor values",
      "'get <sensorname>' returns the values of a given sensor.\r\n",
      NULL, do_sensors_get, NULL },

    { "set", "set sensor values",
      "'set <sensorname> <value-a>[:<value-b>[:<value-c>]]' set the values of a given sensor.\r\n",
      NULL, do_sensors_set, NULL },

    { NULL, NULL, NULL, NULL, NULL, NULL }
};
#endif


/********************************************************************************************/
/********************************************************************************************/
/*****                                                                                 ******/
/*****                           M A I N   C O M M A N D S                             ******/
/*****                                                                                 ******/
/********************************************************************************************/
/********************************************************************************************/

static int
do_kill( ControlClient  client, char*  args )
{
    control_write( client, "OK: killing emulator, bye bye\r\n" );
    exit(0);
}

static const CommandDefRec   main_commands[] =
{
    { "help|h|?", "print a list of commands", NULL, NULL, do_help, NULL },

//    { "event", "simulate hardware events",
//    "allows you to send fake hardware events to the kernel\r\n", NULL,
//    NULL, event_commands },

    { "geo", "Geo-location commands",
      "allows you to change Geo-related settings, or to send GPS NMEA sentences\r\n", NULL,
      NULL, geo_commands },

    { "gsm", "GSM related commands",
      "allows you to change GSM-related settings, or to make a new inbound phone call\r\n", NULL,
      NULL, gsm_commands },

    { "cdma", "CDMA related commands",
      "allows you to change CDMA-related settings\r\n", NULL,
      NULL, cdma_commands },

    { "kill", "kill the emulator instance", NULL, NULL,
      do_kill, NULL },

//    { "power", "power related commands",
//      "allows to change battery and AC power status\r\n", NULL,
//      NULL, power_commands },

    { "quit|exit", "quit control session", NULL, NULL,
      do_quit, NULL },

    { "sms", "SMS related commands",
      "allows you to simulate an inbound SMS\r\n", NULL,
      NULL, sms_commands },

//    { "qemu", "QEMU-specific commands",
//    "allows to connect to the QEMU virtual machine monitor\r\n", NULL,
//    NULL, qemu_commands },

//    { "sensor", "manage emulator sensors",
//      "allows you to request the emulator sensors\r\n", NULL,
//      NULL, sensor_commands },

    { NULL, NULL, NULL, NULL, NULL, NULL }
};

int
control_console_start( int  port )
{
    return control_global_init( &_g_global, port );
}
