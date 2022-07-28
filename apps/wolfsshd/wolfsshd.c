/* wolfsshd.c
 *
 * Copyright (C) 2014-2021 wolfSSL Inc.
 *
 * This file is part of wolfSSH.
 *
 * wolfSSH is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * wolfSSH is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with wolfSSH.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
    #include <config.h>
#endif

#ifdef WOLFSSH_SSHD

#include <wolfssh/ssh.h>
#include <wolfssh/internal.h>
#include <wolfssh/log.h>
#include <wolfssl/wolfcrypt/wc_port.h>
#include <wolfssl/wolfcrypt/error-crypt.h>
#include <wolfssl/wolfcrypt/logging.h>
#include <wolfssl/wolfcrypt/asn_public.h>

#define WOLFSSH_TEST_SERVER
#include <wolfssh/test.h>

#include "configuration.h"
#include "auth.h"

#include <signal.h>

#ifdef NO_INLINE
    #include <wolfssh/misc.h>
#else
    #define WOLFSSH_MISC_INCLUDED
    #include "src/misc.c"
#endif

#ifndef WOLFSSHD_TIMEOUT
    #define WOLFSSHD_TIMEOUT 1
#endif

#ifdef WOLFSSH_SHELL
    #ifdef HAVE_PTY_H
        #include <pty.h>
    #endif
    #ifdef HAVE_UTIL_H
        #include <util.h>
    #endif
    #ifdef HAVE_TERMIOS_H
        #include <termios.h>
    #endif
    #include <pwd.h>
    #include <signal.h>
#if defined(__QNX__) || defined(__QNXNTO__)
    #include <errno.h>
    #include <unix.h>
#else
    #include <sys/errno.h>
#endif

    static volatile int ChildRunning = 0;
    static void ChildSig(int sig)
    {
        (void)sig;
        ChildRunning = 0;
    }
#endif /* WOLFSSH_SHELL */

static volatile byte debugMode = 0; /* default to off */

/* catch interrupts and close down gracefully */
static volatile byte quit = 0;
static const char defaultBanner[] = "wolfSSHD\n";

/* Initial connection information to pass on to threads/forks */
typedef struct WOLFSSHD_CONNECTION {
    WOLFSSH_CTX*   ctx;
    WOLFSSHD_AUTH* auth;
    int            fd;
} WOLFSSHD_CONNECTION;

static void ShowUsage(void)
{
    printf("wolfsshd %s\n", LIBWOLFSSH_VERSION_STRING);
    printf(" -?             display this help and exit\n");
    printf(" -f <file name> Configuration file to use, default is /usr/local/etc/ssh/sshd_config\n");
    printf(" -p <int>       Port number to listen on\n");
    printf(" -d             Turn on debug mode\n");
    printf(" -h <file name> host private key file to use\n");
}

static void interruptCatch(int in)
{
    (void)in;
    printf("Closing down wolfSSHD\n");
    quit = 1;
}

static void wolfSSHDLoggingCb(enum wolfSSH_LogLevel lvl, const char *const str)
{
    if (debugMode) {
        fprintf(stderr, "[PID %d]: %s\n", getpid(), str);
    }
    (void)lvl;
}

static int SetupCTX(WOLFSSHD_CONFIG* conf, WOLFSSH_CTX** ctx)
{
    int ret = WS_SUCCESS;
    const char* banner;
    DerBuffer* der = NULL;
    byte* privBuf;
    word32 privBufSz;

    /* create a new WOLFSSH_CTX */
    *ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_SERVER, NULL);
    if (ctx == NULL) {
        wolfSSH_Log(WS_LOG_ERROR, "[SSHD] Couldn't allocate SSH CTX data.");
        ret = WS_MEMORY_E;
    }

    /* setup authority callback for checking peer connections */
    if (ret == WS_SUCCESS) {
        wolfSSH_SetUserAuth(*ctx, DefaultUserAuth);
    }

    /* set banner to display on connection */
    if (ret == WS_SUCCESS) {
        banner = wolfSSHD_ConfigGetBanner(conf);
        if (banner == NULL) {
            banner = defaultBanner;
        }
        wolfSSH_CTX_SetBanner(*ctx, banner);
    }

#ifdef WOLFSSH_AGENT
    /* check if using an agent is enabled */
    /* TODO: missing */
/*
    if (ret == WS_SUCCESS) {
        wolfSSH_CTX_set_agent_cb(ctx, wolfSSH_AGENT_DefaultActions, NULL);
    }
*/
#endif

#ifdef WOLFSSH_FWD
    /* check if port forwarding is enabled */
    /* TODO: missing */
/*
    if (ret == WS_SUCCESS) {
        wolfSSH_CTX_SetFwdCb(ctx, wolfSSH_FwdDefaultActions, NULL);
    }
*/
#endif

    /* Load in host private key */
    if (ret == WS_SUCCESS) {

        char* hostKey = wolfSSHD_ConfigGetHostKeyFile(conf);

        if (hostKey == NULL) {
            wolfSSH_Log(WS_LOG_ERROR, "[SSHD] No host private key set");
            ret = WS_BAD_ARGUMENT;
        }
        else {
            FILE* f;

            f = XFOPEN(hostKey, "rb");
            if (f == NULL) {
                wolfSSH_Log(WS_LOG_ERROR,
                        "[SSHD] Unable to open host private key");
                ret = WS_BAD_ARGUMENT;
            }
            else {
                byte* data;
                int   dataSz = 4096;

                data = (byte*)WMALLOC(dataSz, NULL, 0);

                dataSz = (int)XFREAD(data, 1, dataSz, f);
                XFCLOSE(f);

                if (wc_PemToDer(data, dataSz, PRIVATEKEY_TYPE, &der, NULL,
                                NULL, NULL) != 0) {
                    wolfSSH_Log(WS_LOG_DEBUG, "[SSHD] Failed to convert host "
                                "private key from PEM. Assuming key in DER "
                                "format.");
                    privBuf = data;
                    privBufSz = dataSz;
                }
                else {
                    privBuf = der->buffer;
                    privBufSz = der->length;
                }

                if (wolfSSH_CTX_UsePrivateKey_buffer(*ctx, privBuf, privBufSz,
                                                     WOLFSSH_FORMAT_ASN1) < 0) {
                    wolfSSH_Log(WS_LOG_ERROR,
                        "[SSHD] Failed to use host private key.");
                    ret = WS_BAD_ARGUMENT;
                }

                WFREE(data, NULL, 0);
                wc_FreeDer(&der);
            }
        }
    }

    /* Load in host public key */
//    {
//        if (userPubKey) {
//            byte* userBuf = NULL;
//            word32 userBufSz = 0;
//
//            /* get the files size */
//            load_file(userPubKey, NULL, &userBufSz);
//
//            /* create temp buffer and load in file */
//            if (userBufSz == 0) {
//                fprintf(stderr, "Couldn't find size of file %s.\n", userPubKey);
//                WEXIT(EXIT_FAILURE);
//            }
//
//            userBuf = (byte*)WMALLOC(userBufSz, NULL, 0);
//            if (userBuf == NULL) {
//                fprintf(stderr, "WMALLOC failed\n");
//                WEXIT(EXIT_FAILURE);
//            }
//            load_file(userPubKey, userBuf, &userBufSz);
//            LoadPublicKeyBuffer(userBuf, userBufSz, &pwMapList);
//        }
//
//        bufSz = (word32)WSTRLEN(samplePasswordBuffer);
//        WMEMCPY(keyLoadBuf, samplePasswordBuffer, bufSz);
//        keyLoadBuf[bufSz] = 0;
//        LoadPasswordBuffer(keyLoadBuf, bufSz, &pwMapList);
//
//        if (userEcc) {
//        #ifndef WOLFSSH_NO_ECC
//            bufName = samplePublicKeyEccBuffer;
//        #endif
//        }
//        else {
//        #ifndef WOLFSSH_NO_RSA
//            bufName = samplePublicKeyRsaBuffer;
//        #endif
//        }
//        if (bufName != NULL) {
//            bufSz = (word32)WSTRLEN(bufName);
//            WMEMCPY(keyLoadBuf, bufName, bufSz);
//            keyLoadBuf[bufSz] = 0;
//            LoadPublicKeyBuffer(keyLoadBuf, bufSz, &pwMapList);
//        }
//
//        bufSz = (word32)WSTRLEN(sampleNoneBuffer);
//        WMEMCPY(keyLoadBuf, sampleNoneBuffer, bufSz);
//        keyLoadBuf[bufSz] = 0;
//        LoadNoneBuffer(keyLoadBuf, bufSz, &pwMapList);
//
//        #ifdef WOLFSSH_SMALL_STACK
//            WFREE(keyLoadBuf, NULL, 0);
//        #endif
//    }

    /* Set allowed connection type, i.e. public key / password */

    return ret;
}


#ifdef WOLFSSH_SFTP
#define TEST_SFTP_TIMEOUT 1

/* handle SFTP operations
 * returns 0 on success
 */
static int SFTP_Subsystem(WOLFSSH* ssh, WOLFSSHD_CONNECTION* conn)
{
    byte tmp[1];
    int ret   = WS_SUCCESS;
    int error = WS_SUCCESS;
    WS_SOCKET_T sockfd;
    int select_ret = 0;

    sockfd = (WS_SOCKET_T)wolfSSH_get_fd(ssh);
    do {
//        if (threadCtx->nonBlock) {
//            if (error == WS_WANT_READ)
//                printf("... sftp server would read block\n");
//            else if (error == WS_WANT_WRITE)
//                printf("... sftp server would write block\n");
//        }

        if (wolfSSH_stream_peek(ssh, tmp, 1) > 0) {
            select_ret = WS_SELECT_RECV_READY;
        }
        else {
            select_ret = tcp_select(sockfd, TEST_SFTP_TIMEOUT);
        }

        if (select_ret == WS_SELECT_RECV_READY ||
            select_ret == WS_SELECT_ERROR_READY ||
            error == WS_WANT_WRITE)
        {
            ret = wolfSSH_SFTP_read(ssh);
            error = wolfSSH_get_error(ssh);
        }
        else if (select_ret == WS_SELECT_TIMEOUT)
            error = WS_WANT_READ;
        else
            error = WS_FATAL_ERROR;

        if (error == WS_WANT_READ || error == WS_WANT_WRITE ||
            error == WS_CHAN_RXD || error == WS_REKEYING ||
            error == WS_WINDOW_FULL)
            ret = error;

        if (ret == WS_FATAL_ERROR && error == 0) {
            WOLFSSH_CHANNEL* channel =
                wolfSSH_ChannelNext(ssh, NULL);
            if (channel && wolfSSH_ChannelGetEof(channel)) {
                ret = 0;
                break;
            }
        }
    } while (ret != WS_FATAL_ERROR);

    (void)conn;
    return ret;
}
#endif


#ifdef WOLFSSH_SHELL
static int SHELL_Subsystem(WOLFSSHD_CONNECTION* conn, WOLFSSH* ssh)
{
    WS_SOCKET_T sshFd = 0;
    int rc;
    const char *userName;
    struct passwd *p_passwd;
    WS_SOCKET_T childFd = 0;
    pid_t childPid;
#ifndef EXAMPLE_BUFFER_SZ
    #define EXAMPLE_BUFFER_SZ 4096
#endif
    byte shellBuffer[EXAMPLE_BUFFER_SZ];
    byte channelBuffer[EXAMPLE_BUFFER_SZ];

    userName = wolfSSH_GetUsername(ssh);

    /* temporarily elevate permissions to get users information */
    if (wolfSSHD_AuthRaisePermissions(conn->auth) != 0) {
        wolfSSH_Log(WS_LOG_ERROR, "[SSHD] Failure to raise permissions for auth"); 
        return WS_FATAL_ERROR;
    }

    p_passwd = getpwnam((const char *)userName);
    if (p_passwd == NULL) {
        /* Not actually a user on the system. */
        wolfSSH_Log(WS_LOG_ERROR, "[SSHD] Invalid user name found");
        if (wolfSSHD_AuthReducePermissions(conn->auth) != 0) {
            /* stop everything if not able to reduce permissions level */
            exit(1);
        }

        return WS_FATAL_ERROR;
    }

    ChildRunning = 1;
    childPid = forkpty(&childFd, NULL, NULL, NULL);
    if (wolfSSHD_AuthReducePermissions(conn->auth) != 0) {
        /* stop everything if not able to reduce permissions level */
        wolfSSH_Log(WS_LOG_ERROR, "[SSHD] Issue reducing permissions level,"
            " exiting now");
        exit(1);
    }

    if (childPid < 0) {
        /* forkpty failed, so return */
        ChildRunning = 0;
        wolfSSH_Log(WS_LOG_ERROR, "[SSHD] Issue creating new forkpty");
        return WS_FATAL_ERROR;
    }
    else if (childPid == 0) {
        /* Child process */
        const char *args[] = {"-sh", NULL};
        char cmd[80];

        signal(SIGINT, SIG_DFL);

        setgid(p_passwd->pw_gid);
        setuid(p_passwd->pw_uid);
        if (system("env") != 0) {
            printf("0 return value from system call\n");
        }

        setenv("HOME", p_passwd->pw_dir, 1);
        setenv("LOGNAME", p_passwd->pw_name, 1);

        /* @TODO this needs reworked, can just exit into root */
        WMEMSET(cmd, 0, sizeof(cmd));
        XSNPRINTF(cmd, sizeof(cmd), "su %s", userName);
        printf("executing command [%s]\n", cmd);
        system(cmd);
        rc = chdir(p_passwd->pw_dir);
        if (rc != 0) {
            return WS_FATAL_ERROR;
        }

        execv("/bin/sh", (char **)args);
    }
    sshFd = wolfSSH_get_fd(ssh);

    struct termios tios;
    word32 shellChannelId = 0;
    signal(SIGCHLD, ChildSig);

    rc = tcgetattr(childFd, &tios);
    if (rc != 0) {
        return WS_FATAL_ERROR;
    }
    rc = tcsetattr(childFd, TCSAFLUSH, &tios);
    if (rc != 0) {
        return WS_FATAL_ERROR;
    }

    while (ChildRunning) {
        byte tmp[2];
        fd_set readFds;
        WS_SOCKET_T maxFd;
        int cnt_r;
        int cnt_w;
        int pending = 0;

        FD_ZERO(&readFds);
        FD_SET(sshFd, &readFds);
        maxFd = sshFd;

        FD_SET(childFd, &readFds);
        if (childFd > maxFd)
            maxFd = childFd;

        if (wolfSSH_stream_peek(ssh, tmp, 1) <= 0) {
            rc = select((int)maxFd + 1, &readFds, NULL, NULL, NULL);
            if (rc == -1)
                break;
        }
        else {
            pending = 1; /* found some pending SSH data */
        }

        if (pending || FD_ISSET(sshFd, &readFds)) {
            word32 lastChannel = 0;

            /* The following tries to read from the first channel inside
               the stream. If the pending data in the socket is for
               another channel, this will return an error with id
               WS_CHAN_RXD. That means the agent has pending data in its
               channel. The additional channel is only used with the
               agent. */
            cnt_r = wolfSSH_worker(ssh, &lastChannel);
            if (cnt_r < 0) {
                rc = wolfSSH_get_error(ssh);
                if (rc == WS_CHAN_RXD) {
                    if (lastChannel == shellChannelId) {
                        cnt_r = wolfSSH_ChannelIdRead(ssh, shellChannelId,
                                channelBuffer,
                                sizeof channelBuffer);
                        if (cnt_r <= 0)
                            break;
                        cnt_w = (int)write(childFd,
                                channelBuffer, cnt_r);
                        if (cnt_w <= 0)
                            break;
                    }
                }
                else if (rc == WS_CHANNEL_CLOSED) {
                    continue;
                }
                else if (rc != WS_WANT_READ) {
                    break;
                }
            }
        }

        if (FD_ISSET(childFd, &readFds)) {
            cnt_r = (int)read(childFd, shellBuffer, sizeof shellBuffer);
            /* This read will return 0 on EOF */
            if (cnt_r <= 0) {
                int err = errno;
                if (err != EAGAIN) {
                    break;
                }
            }
            else {
                if (cnt_r > 0) {
                    cnt_w = wolfSSH_ChannelIdSend(ssh, shellChannelId,
                            shellBuffer, cnt_r);
                    if (cnt_w < 0)
                        break;
                }
            }
        }
    }

    (void)conn;
    return WS_SUCCESS;
}
#endif

static __thread int timeOut = 0;
static void alarmCatch(int signum)
{
    wolfSSH_Log(WS_LOG_ERROR, "[SSHD] Failed login within grace period");
    timeOut = 1;
    (void)signum;
}

/* handle wolfSSH accept and directing to correct subsystem */
static void* HandleConnection(void* arg)
{
    int ret = WS_SUCCESS;

    WOLFSSHD_CONNECTION* conn = NULL;
    WOLFSSH* ssh = NULL;

    if (arg == NULL) {
        ret = WS_BAD_ARGUMENT;
    }

    if (ret == WS_SUCCESS) {
        conn = (WOLFSSHD_CONNECTION*)arg;
        ssh = wolfSSH_new(conn->ctx);
        if (ssh == NULL) {
            wolfSSH_Log(WS_LOG_ERROR,
                "[SSHD] Failed to create new WOLFSSH struct");
            ret = -1;
        }
    }

    if (ret == WS_SUCCESS) {
        int error;
        int select_ret = 0;
        long graceTime;

        wolfSSH_set_fd(ssh, conn->fd);
        wolfSSH_SetUserAuthCtx(ssh, conn->auth);

        /* set alarm for login grace time */
        graceTime = wolfSSHD_AuthGetGraceTime(conn->auth);
        if (graceTime > 0) {
            signal(SIGALRM, alarmCatch);
            alarm(graceTime);
        }

        ret = wolfSSH_accept(ssh);
        error = wolfSSH_get_error(ssh);
        while (timeOut == 0 && (ret != WS_SUCCESS
                && ret != WS_SCP_COMPLETE && ret != WS_SFTP_COMPLETE)
                && (error == WS_WANT_READ || error == WS_WANT_WRITE)) {

            select_ret = tcp_select(conn->fd, 1);
            if (select_ret == WS_SELECT_RECV_READY  ||
                select_ret == WS_SELECT_ERROR_READY ||
                error      == WS_WANT_WRITE)
            {
                ret = wolfSSH_accept(ssh);
                error = wolfSSH_get_error(ssh);
            }
            else if (select_ret == WS_SELECT_TIMEOUT)
                error = WS_WANT_READ;
            else
                error = WS_FATAL_ERROR;
        }

        if (ret != WS_SUCCESS && ret != WS_SFTP_COMPLETE) {
            wolfSSH_Log(WS_LOG_ERROR,
                "[SSHD] Failed to accept WOLFSSH connection");
        }
    }

    if (ret == WS_SUCCESS || ret == WS_SFTP_COMPLETE) {
        switch (wolfSSH_GetSessionType(ssh)) {
            case WOLFSSH_SESSION_SHELL:
            #ifdef WOLFSSH_SHELL
                if (ret == WS_SUCCESS) {
                    wolfSSH_Log(WS_LOG_INFO, "[SSHD] Entering new shell");
                    SHELL_Subsystem(conn, ssh);
                }
            #else
                wolfSSH_Log(WS_LOG_ERROR,
                    "[SSHD] Shell support is disabled");
                ret = WS_NOT_COMPILED;
            #endif
                break;

            case WOLFSSH_SESSION_SUBSYSTEM:
                /* test for known subsystems */
                switch (ret) {
                    case WS_SFTP_COMPLETE:
                    #ifdef WOLFSSH_SFTP
                        ret = SFTP_Subsystem(ssh, conn);
                    #else
                        err_sys("SFTP not compiled in. Please use --enable-sftp");
                    #endif
                        break;

                    default:
                        wolfSSH_Log(WS_LOG_ERROR,
                            "[SSHD] Unknown or build not supporting subsystem"
                            " found [%s]", wolfSSH_GetSessionCommand(ssh));
                        ret = WS_NOT_COMPILED;
                }
                break;

            case WOLFSSH_SESSION_UNKNOWN:
            case WOLFSSH_SESSION_EXEC:
            case WOLFSSH_SESSION_TERMINAL:
            default:
                wolfSSH_Log(WS_LOG_ERROR,
                    "[SSHD] Unknown or build not supporting session type found");
                ret = WS_NOT_COMPILED;
        }
    }

    wolfSSH_Log(WS_LOG_INFO, "[SSHD] Attempting to close down connection");
    wolfSSH_shutdown(ssh);
    wolfSSH_free(ssh);
    if (conn != NULL) {
        WCLOSESOCKET(conn->fd);
    }
    return NULL;
}


/* returns WS_SUCCESS on success */
static int NewConnection(WOLFSSHD_CONNECTION* conn)
{
    int pd;
    int ret = WS_SUCCESS;

    pd = fork();
    if (pd < 0) {
        wolfSSH_Log(WS_LOG_ERROR, "[SSHD] Issue spawning new process");
        ret = -1;
    }

    if (pd == 0) {
        /* child process */
        (void)HandleConnection((void*)conn);
    }
    else {
        wolfSSH_Log(WS_LOG_INFO, "[SSHD] Spawned new process %d\n", pd);
    }

    return ret;
}


/* return non zero value for a pending connection */
static int PendingConnection(WS_SOCKET_T fd)
{
    int ret;
    struct timeval t;
    fd_set r, w, e;
    WS_SOCKET_T nfds = fd + 1;

    t.tv_usec = 0;
    t.tv_sec  = WOLFSSHD_TIMEOUT;

    FD_ZERO(&r);
    FD_ZERO(&w);
    FD_ZERO(&e);

    FD_SET(fd, &r);
    ret = select(nfds, &r, &w, &e, &t);
    if (ret < 0) {
        /* a socket level issue happend */
        printf("Error waiting for connection on socket\n");
        quit = 1;
        ret  = 0;
    }
    else if (ret > 0) {
        if (FD_ISSET(fd, &r)) {
            printf("Connection found\n");
        }
        else {
            printf("Found write or error data\n");
            ret = 0; /* nothing to read */
        }
    }
    //    printf("Timeout waiting for connection\n");
    return ret;
}


int   myoptind = 0;
char* myoptarg = NULL;

int main(int argc, char** argv)
{
    int ret  = WS_SUCCESS;
    word16 port = 0;
    WS_SOCKET_T listenFd = 0;
    int ch;
    WOLFSSHD_CONFIG* conf = NULL;
    WOLFSSHD_AUTH*   auth = NULL;
    WOLFSSH_CTX* ctx = NULL;

    const char* configFile  = "/usr/local/etc/ssh/sshd_config";
    const char* hostKeyFile = NULL;

    signal(SIGINT, interruptCatch);

    wolfSSH_SetLoggingCb(wolfSSHDLoggingCb);
#ifdef DEBUG_WOLFSSH
    wolfSSH_Debugging_ON();
#endif
#ifdef DEBUG_WOLFSSL
    wolfSSL_Debugging_ON();
#endif

    if (ret == WS_SUCCESS) {
        wolfSSH_Init();
    }

    if (ret == WS_SUCCESS) {
        conf = wolfSSHD_NewConfig(NULL);
        if (conf == NULL) {
            ret = WS_MEMORY_E;
        }
    }

    while ((ch = mygetopt(argc, argv, "?f:p:h:d")) != -1) {
        switch (ch) {
            case 'f':
                configFile = myoptarg;
                break;

            case 'p':
                if (ret == WS_SUCCESS) {
                    ret = XATOI(myoptarg);
                    if (ret < 0) {
                        printf("Issue parsing port number %s\n", myoptarg);
                        ret = WS_BAD_ARGUMENT;
                    }
                    else {
                        if (ret <= (word16)-1) {
                            port = (word16)ret;
                        }
                        else {
                            printf("Port number %d too big.\n", ret);
                            ret = WS_BAD_ARGUMENT;
                        }
                    }
                }
                break;

            case 'h':
                hostKeyFile = myoptarg;
                break;

            case 'd':
                debugMode = 1; /* turn on debug mode */
                break;

            case '?':
                ShowUsage();
                return WS_SUCCESS;

            default:
                ShowUsage();
                return WS_SUCCESS;
        }
    }

    if (ret == WS_SUCCESS) {
        ret = wolfSSHD_ConfigLoad(conf, configFile);
        if (ret != WS_SUCCESS)
            printf("Error reading in configure file %s\n", configFile);
    }

    /* port was not overridden with argument, read from config file */
    if (port == 0) {
        port = wolfSSHD_ConfigGetPort(conf);
    }

    /* check if host key file was passed in */
    if (hostKeyFile != NULL) {
        wolfSSHD_ConfigSetHostKeyFile(conf, hostKeyFile);
    }

    wolfSSH_Log(WS_LOG_INFO, "[SSHD] Starting wolfSSH SSHD application");

    if (ret == WS_SUCCESS) {
        ret = SetupCTX(conf, &ctx);
    }
    else {
        /* TODO: handle error. */
    }

    if (ret == WS_SUCCESS) {
        auth = wolfSSHD_AuthCreateUser(NULL, conf);
        if (auth == NULL) {
            wolfSSH_Log(WS_LOG_ERROR, "[SSHD] Issue creating auth struct");
            ret = WS_MEMORY_E;
        }
    }

    /* seperate privlage permisions */
    if (ret == WS_SUCCESS) {
        if (wolfSSHD_AuthReducePermissions(auth) != 0) {
            wolfSSH_Log(WS_LOG_INFO, "[SSHD] Error lowering permissions level");
            ret = WS_FATAL_ERROR;
        }
    }

    if (ret == WS_SUCCESS) {
        wolfSSH_Log(WS_LOG_INFO, "[SSHD] Starting to listen on port %d", port);
        tcp_listen(&listenFd, &port, 1);
        wolfSSH_Log(WS_LOG_INFO, "[SSHD] Listening on port %d", port);

        /* wait for incoming connections and fork them off */
        while (ret == WS_SUCCESS && quit == 0) {
            WOLFSSHD_CONNECTION conn;
        #ifdef WOLFSSL_NUCLEUS
            struct addr_struct clientAddr;
        #else
            SOCKADDR_IN_T clientAddr;
            socklen_t     clientAddrSz = sizeof(clientAddr);
        #endif
            conn.auth = auth;

            /* wait for a connection */
            if (PendingConnection(listenFd)) {
                conn.ctx = ctx;
            #ifdef WOLFSSL_NUCLEUS
                conn.fd = NU_Accept(listenFd, &clientAddr, 0);
            #else
                conn.fd = accept(listenFd, (struct sockaddr*)&clientAddr,
                                                             &clientAddrSz);
            #endif

                {
                    #ifdef USE_WINDOWS_API
                        unsigned long blocking = 1;
                        int ret = ioctlsocket(conn.fd, FIONBIO, &blocking);
                        if (ret == SOCKET_ERROR)
                            err_sys("ioctlsocket failed");
                    #elif defined(WOLFSSL_MDK_ARM) || defined(WOLFSSL_KEIL_TCP_NET) \
                        || defined (WOLFSSL_TIRTOS)|| defined(WOLFSSL_VXWORKS) || \
                        defined(WOLFSSL_NUCLEUS)
                         /* non blocking not supported, for now */
                    #else
                        int flags = fcntl(conn.fd, F_GETFL, 0);
                        if (flags < 0)
                            err_sys("fcntl get failed");
                        flags = fcntl(conn.fd, F_SETFL, flags | O_NONBLOCK);
                        if (flags < 0)
                            err_sys("fcntl set failed");
                    #endif
                }
                ret = NewConnection(&conn);
            }
        }
    }

    wolfSSHD_ConfigFree(conf);
    wolfSSHD_AuthFreeUser(auth);
    wolfSSH_Cleanup();

    return 0;
}
#endif /* WOLFSSH_SSHD */
